/******************************************************************************
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "btif_av"

#define BLUETOOTH_RTK 1
#include "btif_av.h"

#include <assert.h>
#include <string.h>

#include <system/audio.h>
#include <hardware/bluetooth.h>
#include <hardware/bt_av.h>

#include "bt_utils.h"
#include "bta_api.h"
#include "btif_media.h"
#include "btif_profile_queue.h"
#include "btif_util.h"
#include "btu.h"
#include "bt_common.h"
#ifdef BLUETOOTH_RTK
#include "btif_storage.h"
#include "uuid.h"
#endif
#include "osi/include/allocator.h"

/*****************************************************************************
**  Constants & Macros
******************************************************************************/
#define BTIF_AV_SERVICE_NAME "Advanced Audio"
#define BTIF_AVK_SERVICE_NAME "Advanced Audio Sink"

#define BTIF_TIMEOUT_AV_OPEN_ON_RC_MS  (2 * 1000)

typedef enum {
    BTIF_AV_STATE_IDLE = 0x0,
    BTIF_AV_STATE_OPENING,
    BTIF_AV_STATE_OPENED,
    BTIF_AV_STATE_STARTED,
    BTIF_AV_STATE_CLOSING
} btif_av_state_t;

/* Should not need dedicated suspend state as actual actions are no
   different than open state. Suspend flags are needed however to prevent
   media task from trying to restart stream during remote suspend or while
   we are in the process of a local suspend */

#define BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING 0x1
#define BTIF_AV_FLAG_REMOTE_SUSPEND        0x2
#define BTIF_AV_FLAG_PENDING_START         0x4
#define BTIF_AV_FLAG_PENDING_STOP          0x8

/*****************************************************************************
**  Local type definitions
******************************************************************************/

typedef struct
{
    tBTA_AV_HNDL bta_handle;
    bt_bdaddr_t peer_bda;
    btif_sm_handle_t sm_handle;
    UINT8 flags;
    tBTA_AV_EDR edr;
    UINT8 peer_sep;  /* sep type of peer device */
#ifdef BLUETOOTH_RTK
    uint16_t uuid;
#endif
} btif_av_cb_t;

typedef struct
{
    bt_bdaddr_t *target_bda;
    uint16_t uuid;
} btif_av_connect_req_t;

typedef struct
{
    int sample_rate;
    int channel_count;
    bt_bdaddr_t peer_bd;
} btif_av_sink_config_req_t;

/*****************************************************************************
**  Static variables
******************************************************************************/
static btav_callbacks_t *bt_av_src_callbacks = NULL;
static btav_callbacks_t *bt_av_sink_callbacks = NULL;
#ifdef BLUETOOTH_RTK
static btif_av_cb_t btif_av_cb = {0, {{0}}, 0, 0, 0, 0,0};
#else
static btif_av_cb_t btif_av_cb = {0, {{0}}, 0, 0, 0, 0};
#endif
static alarm_t *av_open_on_rc_timer = NULL;

/* both interface and media task needs to be ready to alloc incoming request */
#define CHECK_BTAV_INIT() if (((bt_av_src_callbacks == NULL) &&(bt_av_sink_callbacks == NULL)) \
        || (btif_av_cb.sm_handle == NULL))\
{\
     BTIF_TRACE_WARNING("%s: BTAV not initialized", __FUNCTION__);\
     return BT_STATUS_NOT_READY;\
}\
else\
{\
     BTIF_TRACE_EVENT("%s", __FUNCTION__);\
}

/* Helper macro to avoid code duplication in the state machine handlers */
#define CHECK_RC_EVENT(e, d) \
    case BTA_AV_RC_OPEN_EVT: \
    case BTA_AV_RC_CLOSE_EVT: \
    case BTA_AV_REMOTE_CMD_EVT: \
    case BTA_AV_VENDOR_CMD_EVT: \
    case BTA_AV_META_MSG_EVT: \
    case BTA_AV_RC_FEAT_EVT: \
    case BTA_AV_REMOTE_RSP_EVT: \
    { \
         btif_rc_handler(e, d);\
    }break; \

static BOOLEAN btif_av_state_idle_handler(btif_sm_event_t event, void *data);
static BOOLEAN btif_av_state_opening_handler(btif_sm_event_t event, void *data);
static BOOLEAN btif_av_state_opened_handler(btif_sm_event_t event, void *data);
static BOOLEAN btif_av_state_started_handler(btif_sm_event_t event, void *data);
static BOOLEAN btif_av_state_closing_handler(btif_sm_event_t event, void *data);
#ifdef BLUETOOTH_RTK
static BOOLEAN btif_av_get_peer_role(bt_bdaddr_t *bd_addr);
#endif
static const btif_sm_handler_t btif_av_state_handlers[] =
{
    btif_av_state_idle_handler,
    btif_av_state_opening_handler,
    btif_av_state_opened_handler,
    btif_av_state_started_handler,
    btif_av_state_closing_handler
};

static void btif_av_event_free_data(btif_sm_event_t event, void *p_data);

/*************************************************************************
** Extern functions
*************************************************************************/
extern void btif_rc_handler(tBTA_AV_EVT event, tBTA_AV *p_data);
extern BOOLEAN btif_rc_get_connected_peer(BD_ADDR peer_addr);
extern UINT8 btif_rc_get_connected_peer_handle(void);
extern void btif_rc_check_handle_pending_play (BD_ADDR peer_addr, BOOLEAN bSendToApp);

extern fixed_queue_t *btu_general_alarm_queue;

/*****************************************************************************
** Local helper functions
******************************************************************************/

const char *dump_av_sm_state_name(btif_av_state_t state)
{
    switch (state)
    {
        CASE_RETURN_STR(BTIF_AV_STATE_IDLE)
        CASE_RETURN_STR(BTIF_AV_STATE_OPENING)
        CASE_RETURN_STR(BTIF_AV_STATE_OPENED)
        CASE_RETURN_STR(BTIF_AV_STATE_STARTED)
        CASE_RETURN_STR(BTIF_AV_STATE_CLOSING)
        default: return "UNKNOWN_STATE";
    }
}

const char *dump_av_sm_event_name(btif_av_sm_event_t event)
{
    switch((int)event)
    {
        CASE_RETURN_STR(BTA_AV_ENABLE_EVT)
        CASE_RETURN_STR(BTA_AV_REGISTER_EVT)
        CASE_RETURN_STR(BTA_AV_OPEN_EVT)
        CASE_RETURN_STR(BTA_AV_CLOSE_EVT)
        CASE_RETURN_STR(BTA_AV_START_EVT)
        CASE_RETURN_STR(BTA_AV_STOP_EVT)
        CASE_RETURN_STR(BTA_AV_PROTECT_REQ_EVT)
        CASE_RETURN_STR(BTA_AV_PROTECT_RSP_EVT)
        CASE_RETURN_STR(BTA_AV_RC_OPEN_EVT)
        CASE_RETURN_STR(BTA_AV_RC_CLOSE_EVT)
        CASE_RETURN_STR(BTA_AV_REMOTE_CMD_EVT)
        CASE_RETURN_STR(BTA_AV_REMOTE_RSP_EVT)
        CASE_RETURN_STR(BTA_AV_VENDOR_CMD_EVT)
        CASE_RETURN_STR(BTA_AV_VENDOR_RSP_EVT)
        CASE_RETURN_STR(BTA_AV_RECONFIG_EVT)
        CASE_RETURN_STR(BTA_AV_SUSPEND_EVT)
        CASE_RETURN_STR(BTA_AV_PENDING_EVT)
        CASE_RETURN_STR(BTA_AV_META_MSG_EVT)
        CASE_RETURN_STR(BTA_AV_REJECT_EVT)
        CASE_RETURN_STR(BTA_AV_RC_FEAT_EVT)
        CASE_RETURN_STR(BTA_AV_OFFLOAD_START_RSP_EVT)
        CASE_RETURN_STR(BTIF_SM_ENTER_EVT)
        CASE_RETURN_STR(BTIF_SM_EXIT_EVT)
        CASE_RETURN_STR(BTIF_AV_CONNECT_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_DISCONNECT_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_START_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_STOP_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_SUSPEND_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_SINK_CONFIG_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_OFFLOAD_START_REQ_EVT)
#ifdef USE_AUDIO_TRACK
        CASE_RETURN_STR(BTIF_AV_SINK_FOCUS_REQ_EVT)
#endif
        default: return "UNKNOWN_EVENT";
   }
}

/****************************************************************************
**  Local helper functions
*****************************************************************************/
/*******************************************************************************
**
** Function         btif_initiate_av_open_timer_timeout
**
** Description      Timer to trigger AV open if the remote headset establishes
**                  RC connection w/o AV connection. The timer is needed to IOP
**                  with headsets that do establish AV after RC connection.
**
** Returns          void
**
*******************************************************************************/
static void btif_initiate_av_open_timer_timeout(UNUSED_ATTR void *data)
{
    BD_ADDR peer_addr;
    btif_av_connect_req_t connect_req;

    /* is there at least one RC connection - There should be */
    if (btif_rc_get_connected_peer(peer_addr)) {
       BTIF_TRACE_DEBUG("%s Issuing connect to the remote RC peer", __FUNCTION__);
       /* In case of AVRCP connection request, we will initiate SRC connection */
       connect_req.target_bda = (bt_bdaddr_t*)&peer_addr;
       if(bt_av_sink_callbacks != NULL)
           connect_req.uuid = UUID_SERVCLASS_AUDIO_SINK;
       else if(bt_av_src_callbacks != NULL)
           connect_req.uuid = UUID_SERVCLASS_AUDIO_SOURCE;
       btif_sm_dispatch(btif_av_cb.sm_handle, BTIF_AV_CONNECT_REQ_EVT, (char*)&connect_req);
    }
    else
    {
        BTIF_TRACE_ERROR("%s No connected RC peers", __FUNCTION__);
    }
}

/*****************************************************************************
**  Static functions
******************************************************************************/

/*******************************************************************************
**
** Function         btif_report_connection_state
**
** Description      Updates the components via the callbacks about the connection
**                  state of a2dp connection.
**
** Returns          None
**
*******************************************************************************/
static void btif_report_connection_state(btav_connection_state_t state, bt_bdaddr_t *bd_addr)
{
#ifdef BLUETOOTH_RTK
    bool peerIsSource;
    peerIsSource = btif_av_get_peer_role(bd_addr);
    if (bt_av_sink_callbacks != NULL && (peerIsSource == false || btif_av_cb.uuid == UUID_SERVCLASS_AUDIO_SINK)) {
        HAL_CBACK(bt_av_sink_callbacks, connection_state_cb, state, bd_addr);
     } else if ((bt_av_src_callbacks != NULL) && (btif_av_cb.uuid == UUID_SERVCLASS_AUDIO_SOURCE || peerIsSource == true)) {
        HAL_CBACK(bt_av_src_callbacks, connection_state_cb, state, bd_addr);
    }
#else
    if (bt_av_sink_callbacks != NULL) {
        HAL_CBACK(bt_av_sink_callbacks, connection_state_cb, state, bd_addr);
    } else if (bt_av_src_callbacks != NULL) {
        HAL_CBACK(bt_av_src_callbacks, connection_state_cb, state, bd_addr);
    }
#endif
}

/*******************************************************************************
**
** Function         btif_report_audio_state
**
** Description      Updates the components via the callbacks about the audio
**                  state of a2dp connection. The state is updated when either
**                  the remote ends starts streaming (started state) or whenever
**                  it transitions out of started state (to opened or streaming)
**                  state.
**
** Returns          None
**
*******************************************************************************/
static void btif_report_audio_state(btav_audio_state_t state, bt_bdaddr_t *bd_addr)
{
#ifdef BLUETOOTH_RTK
    if (btif_av_cb.peer_sep == AVDT_TSEP_SRC && bt_av_sink_callbacks != NULL) {
        HAL_CBACK(bt_av_sink_callbacks, audio_state_cb, state, bd_addr);
    } else if (btif_av_cb.peer_sep == AVDT_TSEP_SNK && bt_av_src_callbacks != NULL) {
        HAL_CBACK(bt_av_src_callbacks, audio_state_cb, state, bd_addr);
    }
#else
    if (bt_av_sink_callbacks != NULL) {
        HAL_CBACK(bt_av_sink_callbacks, audio_state_cb, state, bd_addr);
    } else if (bt_av_src_callbacks != NULL) {
        HAL_CBACK(bt_av_src_callbacks, audio_state_cb, state, bd_addr);
    }
#endif
}

/*****************************************************************************
**
** Function     btif_av_state_idle_handler
**
** Description  State managing disconnected AV link
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_idle_handler(btif_sm_event_t event, void *p_data)
{
    BTIF_TRACE_DEBUG("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            /* clear the peer_bda */
            memset(&btif_av_cb.peer_bda, 0, sizeof(bt_bdaddr_t));
            btif_av_cb.flags = 0;
            btif_av_cb.edr = 0;
            btif_a2dp_on_idle();
            break;

        case BTIF_SM_EXIT_EVT:
            break;

        case BTA_AV_ENABLE_EVT:
            break;

        case BTA_AV_REGISTER_EVT:
            btif_av_cb.bta_handle = ((tBTA_AV*)p_data)->registr.hndl;
            break;

        case BTA_AV_PENDING_EVT:
        case BTIF_AV_CONNECT_REQ_EVT:
        {
             if (event == BTIF_AV_CONNECT_REQ_EVT)
             {
                 memcpy(&btif_av_cb.peer_bda, ((btif_av_connect_req_t*)p_data)->target_bda,
                                                                   sizeof(bt_bdaddr_t));
                 BTA_AvOpen(btif_av_cb.peer_bda.address, btif_av_cb.bta_handle,
                    TRUE, BTA_SEC_AUTHENTICATE, ((btif_av_connect_req_t*)p_data)->uuid);
             }
             else if (event == BTA_AV_PENDING_EVT)
             {
                  bdcpy(btif_av_cb.peer_bda.address, ((tBTA_AV*)p_data)->pend.bd_addr);
                  if (bt_av_src_callbacks != NULL)
                  {
                      BTA_AvOpen(btif_av_cb.peer_bda.address, btif_av_cb.bta_handle,
                        TRUE, BTA_SEC_AUTHENTICATE, UUID_SERVCLASS_AUDIO_SOURCE);
                  }
                  if (bt_av_sink_callbacks != NULL)
                  {
                      BTA_AvOpen(btif_av_cb.peer_bda.address, btif_av_cb.bta_handle,
                                 TRUE, BTA_SEC_AUTHENTICATE, UUID_SERVCLASS_AUDIO_SINK);
                  }
             }
             btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_OPENING);
        } break;

        case BTA_AV_RC_OPEN_EVT:
            /* IOP_FIX: Jabra 620 only does RC open without AV open whenever it connects. So
             * as per the AV WP, an AVRC connection cannot exist without an AV connection. Therefore,
             * we initiate an AV connection if an RC_OPEN_EVT is received when we are in AV_CLOSED state.
             * We initiate the AV connection after a small 3s timeout to avoid any collisions from the
             * headsets, as some headsets initiate the AVRC connection first and then
             * immediately initiate the AV connection
             *
             * TODO: We may need to do this only on an AVRCP Play. FixMe
             */

            BTIF_TRACE_DEBUG("BTA_AV_RC_OPEN_EVT received w/o AV");
            alarm_set_on_queue(av_open_on_rc_timer,
                               BTIF_TIMEOUT_AV_OPEN_ON_RC_MS,
                               btif_initiate_av_open_timer_timeout, NULL,
                               btu_general_alarm_queue);
            btif_rc_handler(event, p_data);
            break;

           /*
            * In case Signalling channel is not down
            * and remote started Streaming Procedure
            * we have to handle config and open event in
            * idle_state. We hit these scenarios while running
            * PTS test case for AVRCP Controller
            */
        case BTIF_AV_SINK_CONFIG_REQ_EVT:
        {
            btif_av_sink_config_req_t req;
            // copy to avoid alignment problems
            memcpy(&req, p_data, sizeof(req));

            BTIF_TRACE_WARNING("BTIF_AV_SINK_CONFIG_REQ_EVT %d %d", req.sample_rate,
                    req.channel_count);
            if (bt_av_sink_callbacks != NULL) {
                HAL_CBACK(bt_av_sink_callbacks, audio_config_cb, &(req.peer_bd),
                        req.sample_rate, req.channel_count);
            }
        } break;

        case BTA_AV_OPEN_EVT:
        {
            tBTA_AV *p_bta_data = (tBTA_AV*)p_data;
            btav_connection_state_t state;
            btif_sm_state_t av_state;
            BTIF_TRACE_DEBUG("status:%d, edr 0x%x",p_bta_data->open.status,
                               p_bta_data->open.edr);

            if (p_bta_data->open.status == BTA_AV_SUCCESS)
            {
                 state = BTAV_CONNECTION_STATE_CONNECTED;
                 av_state = BTIF_AV_STATE_OPENED;
                 btif_av_cb.edr = p_bta_data->open.edr;

                 btif_av_cb.peer_sep = p_bta_data->open.sep;
                 btif_a2dp_set_peer_sep(p_bta_data->open.sep);
            }
            else
            {
                BTIF_TRACE_WARNING("BTA_AV_OPEN_EVT::FAILED status: %d",
                                     p_bta_data->open.status );
                state = BTAV_CONNECTION_STATE_DISCONNECTED;
                av_state  = BTIF_AV_STATE_IDLE;
            }

            /* inform the application of the event */
            btif_report_connection_state(state, &(btif_av_cb.peer_bda));
            /* change state to open/idle based on the status */
            btif_sm_change_state(btif_av_cb.sm_handle, av_state);
            if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
            {
                /* if queued PLAY command,  send it now */
                btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr,
                                             (p_bta_data->open.status == BTA_AV_SUCCESS));
            }
            else if (btif_av_cb.peer_sep == AVDT_TSEP_SRC)
            {
                /* if queued PLAY command,  send it now */
                btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr, FALSE);
                /* Bring up AVRCP connection too */
                BTA_AvOpenRc(btif_av_cb.bta_handle);
            }
            btif_queue_advance();
        } break;

        case BTA_AV_REMOTE_CMD_EVT:
        case BTA_AV_VENDOR_CMD_EVT:
        case BTA_AV_META_MSG_EVT:
        case BTA_AV_RC_FEAT_EVT:
        case BTA_AV_REMOTE_RSP_EVT:
            btif_rc_handler(event, (tBTA_AV*)p_data);
            break;

        case BTA_AV_RC_CLOSE_EVT:
            BTIF_TRACE_DEBUG("BTA_AV_RC_CLOSE_EVT: Stopping AV timer.");
            alarm_cancel(av_open_on_rc_timer);
            btif_rc_handler(event, p_data);
            break;

        case BTIF_AV_OFFLOAD_START_REQ_EVT:
            BTIF_TRACE_ERROR("BTIF_AV_OFFLOAD_START_REQ_EVT: Stream not Started IDLE");
            btif_a2dp_on_offload_started(BTA_AV_FAIL);
            break;

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                                dump_av_sm_event_name(event));
            return FALSE;

    }

    return TRUE;
}
/*****************************************************************************
**
** Function        btif_av_state_opening_handler
**
** Description     Intermediate state managing events during establishment
**                 of avdtp channel
**
** Returns         TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_opening_handler(btif_sm_event_t event, void *p_data)
{
    BTIF_TRACE_DEBUG("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            /* inform the application that we are entering connecting state */
            btif_report_connection_state(BTAV_CONNECTION_STATE_CONNECTING, &(btif_av_cb.peer_bda));
            break;

        case BTIF_SM_EXIT_EVT:
            break;

        case BTA_AV_REJECT_EVT:
            BTIF_TRACE_DEBUG(" Received  BTA_AV_REJECT_EVT ");
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;

        case BTA_AV_OPEN_EVT:
        {
            tBTA_AV *p_bta_data = (tBTA_AV*)p_data;
            btav_connection_state_t state;
            btif_sm_state_t av_state;
            BTIF_TRACE_DEBUG("status:%d, edr 0x%x",p_bta_data->open.status,
                               p_bta_data->open.edr);

            if (p_bta_data->open.status == BTA_AV_SUCCESS)
            {
                 state = BTAV_CONNECTION_STATE_CONNECTED;
                 av_state = BTIF_AV_STATE_OPENED;
                 btif_av_cb.edr = p_bta_data->open.edr;

                 btif_av_cb.peer_sep = p_bta_data->open.sep;
                 btif_a2dp_set_peer_sep(p_bta_data->open.sep);
            }
            else
            {
                BTIF_TRACE_WARNING("BTA_AV_OPEN_EVT::FAILED status: %d",
                                     p_bta_data->open.status );
                BD_ADDR peer_addr;
                if ((btif_rc_get_connected_peer(peer_addr))
                    &&(!bdcmp(btif_av_cb.peer_bda.address, peer_addr)))
                {
                    /*
                     * Disconnect AVRCP connection, if
                     * A2DP conneciton failed, for any reason
                     */
                    BTIF_TRACE_WARNING(" Disconnecting AVRCP ");
                    BTA_AvCloseRc(btif_rc_get_connected_peer_handle());
                }
                state = BTAV_CONNECTION_STATE_DISCONNECTED;
                av_state  = BTIF_AV_STATE_IDLE;
            }

            /* inform the application of the event */
            btif_report_connection_state(state, &(btif_av_cb.peer_bda));
            /* change state to open/idle based on the status */
            btif_sm_change_state(btif_av_cb.sm_handle, av_state);
            if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
            {
                /* if queued PLAY command,  send it now */
                btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr,
                                             (p_bta_data->open.status == BTA_AV_SUCCESS));
            }
            else if (btif_av_cb.peer_sep == AVDT_TSEP_SRC)
            {
                /* if queued PLAY command,  send it now */
                btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr, FALSE);
                /* Bring up AVRCP connection too */
                BTA_AvOpenRc(btif_av_cb.bta_handle);
            }
            btif_queue_advance();
        } break;

        case BTIF_AV_SINK_CONFIG_REQ_EVT:
        {
            btif_av_sink_config_req_t req;
            // copy to avoid alignment problems
            memcpy(&req, p_data, sizeof(req));

            BTIF_TRACE_WARNING("BTIF_AV_SINK_CONFIG_REQ_EVT %d %d", req.sample_rate,
                    req.channel_count);
            if (btif_av_cb.peer_sep == AVDT_TSEP_SRC && bt_av_sink_callbacks != NULL) {
                HAL_CBACK(bt_av_sink_callbacks, audio_config_cb, &(btif_av_cb.peer_bda),
                        req.sample_rate, req.channel_count);
            }
        } break;

        case BTIF_AV_CONNECT_REQ_EVT:
            // Check for device, if same device which moved to opening then ignore callback
            if (memcmp ((bt_bdaddr_t*)p_data, &(btif_av_cb.peer_bda),
                sizeof(btif_av_cb.peer_bda)) == 0)
            {
                BTIF_TRACE_DEBUG("%s: Same device moved to Opening state,ignore Connect Req", __func__);
                btif_queue_advance();
                break;
            }
            else
            {
                BTIF_TRACE_DEBUG("%s: Moved from idle by Incoming Connection request", __func__);
                btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, (bt_bdaddr_t*)p_data);
                btif_queue_advance();
                break;
            }

        case BTA_AV_PENDING_EVT:
            // Check for device, if same device which moved to opening then ignore callback
            if (memcmp (((tBTA_AV*)p_data)->pend.bd_addr, &(btif_av_cb.peer_bda),
                sizeof(btif_av_cb.peer_bda)) == 0)
            {
                BTIF_TRACE_DEBUG("%s: Same device moved to Opening state,ignore Pending Req", __func__);
                break;
            }
            else
            {
                BTIF_TRACE_DEBUG("%s: Moved from idle by outgoing Connection request", __func__);
                BTA_AvDisconnect(((tBTA_AV*)p_data)->pend.bd_addr);
                break;
            }

        case BTIF_AV_OFFLOAD_START_REQ_EVT:
            btif_a2dp_on_offload_started(BTA_AV_FAIL);
            BTIF_TRACE_ERROR("BTIF_AV_OFFLOAD_START_REQ_EVT: Stream not Started OPENING");
            break;

        case BTA_AV_CLOSE_EVT:
            btif_a2dp_on_stopped(NULL);
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                    &(btif_av_cb.peer_bda));
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;

        CHECK_RC_EVENT(event, p_data);

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                                dump_av_sm_event_name(event));
            return FALSE;

   }
   return TRUE;
}

/*****************************************************************************
**
** Function        btif_av_state_closing_handler
**
** Description     Intermediate state managing events during closing
**                 of avdtp channel
**
** Returns         TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_closing_handler(btif_sm_event_t event, void *p_data)
{
    BTIF_TRACE_DEBUG("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
            {
                /* immediately stop transmission of frames */
                btif_a2dp_set_tx_flush(TRUE);
                /* wait for audioflinger to stop a2dp */
            }
            if (btif_av_cb.peer_sep == AVDT_TSEP_SRC)
            {
                btif_a2dp_set_rx_flush(TRUE);
            }
            break;

        case BTA_AV_STOP_EVT:
        case BTIF_AV_STOP_STREAM_REQ_EVT:
            if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
            {
              /* immediately flush any pending tx frames while suspend is pending */
              btif_a2dp_set_tx_flush(TRUE);
            }
            if (btif_av_cb.peer_sep == AVDT_TSEP_SRC)
            {
                btif_a2dp_set_rx_flush(TRUE);
            }

            btif_a2dp_on_stopped(NULL);
            break;

        case BTIF_SM_EXIT_EVT:
            break;

        case BTA_AV_CLOSE_EVT:

            /* inform the application that we are disconnecting */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));

            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;

        /* Handle the RC_CLOSE event for the cleanup */
        case BTA_AV_RC_CLOSE_EVT:
            btif_rc_handler(event, (tBTA_AV*)p_data);
            break;

        case BTIF_AV_OFFLOAD_START_REQ_EVT:
            btif_a2dp_on_offload_started(BTA_AV_FAIL);
            BTIF_TRACE_ERROR("BTIF_AV_OFFLOAD_START_REQ_EVT: Stream not Started Closing");
            break;

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                                dump_av_sm_event_name(event));
            return FALSE;
   }
   return TRUE;
}

/*****************************************************************************
**
** Function     btif_av_state_opened_handler
**
** Description  Handles AV events while AVDTP is in OPEN state
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_opened_handler(btif_sm_event_t event, void *p_data)
{
    tBTA_AV *p_av = (tBTA_AV*)p_data;

    BTIF_TRACE_DEBUG("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    if ( (event == BTA_AV_REMOTE_CMD_EVT) && (btif_av_cb.flags & BTIF_AV_FLAG_REMOTE_SUSPEND) &&
         (p_av->remote_cmd.rc_id == BTA_AV_RC_PLAY) )
    {
        BTIF_TRACE_EVENT("%s: Resetting remote suspend flag on RC PLAY", __FUNCTION__);
        btif_av_cb.flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;
    }

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_STOP;
            btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_START;
            break;

        case BTIF_SM_EXIT_EVT:
            btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_START;
            break;

        case BTIF_AV_START_STREAM_REQ_EVT:
            if (btif_av_cb.peer_sep != AVDT_TSEP_SRC)
                btif_a2dp_setup_codec();
            BTA_AvStart();
            btif_av_cb.flags |= BTIF_AV_FLAG_PENDING_START;
            break;

        case BTA_AV_START_EVT:
        {
            BTIF_TRACE_EVENT("BTA_AV_START_EVT status %d, suspending %d, init %d",
                p_av->start.status, p_av->start.suspending, p_av->start.initiator);

            if ((p_av->start.status == BTA_SUCCESS) && (p_av->start.suspending == TRUE))
                return TRUE;

            /* if remote tries to start a2dp when DUT is a2dp source
             * then suspend. In case a2dp is sink and call is active
             * then disconnect the AVDTP channel
             */
            if (!(btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START))
            {
                if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
                {
                    BTIF_TRACE_EVENT("%s: trigger suspend as remote initiated!!", __FUNCTION__);
                    btif_dispatch_sm_event(BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL, 0);
                }
            }

            /*  In case peer is A2DP SRC we do not want to ack commands on UIPC*/
            if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
            {
                if (btif_a2dp_on_started(&p_av->start,
                    ((btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START) != 0)))
                {
                    /* only clear pending flag after acknowledgement */
                    btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_START;
                }
            }

            /* remain in open state if status failed */
            if (p_av->start.status != BTA_AV_SUCCESS)
                return FALSE;

            if (btif_av_cb.peer_sep == AVDT_TSEP_SRC)
            {
                btif_a2dp_set_rx_flush(FALSE); /*  remove flush state, ready for streaming*/
            }

            /* change state to started, send acknowledgement if start is pending */
            if (btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START) {
                if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
                    btif_a2dp_on_started(NULL, TRUE);
                /* pending start flag will be cleared when exit current state */
            }
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_STARTED);

        } break;

        case BTIF_AV_DISCONNECT_REQ_EVT:
            BTA_AvClose(btif_av_cb.bta_handle);
            if (btif_av_cb.peer_sep == AVDT_TSEP_SRC) {
                BTA_AvCloseRc(btif_av_cb.bta_handle);
            }

            /* inform the application that we are disconnecting */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTING, &(btif_av_cb.peer_bda));
            break;

        case BTA_AV_CLOSE_EVT:
             /* avdtp link is closed */
            btif_a2dp_on_stopped(NULL);

            /* inform the application that we are disconnected */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));

            /* change state to idle, send acknowledgement if start is pending */
            if (btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START) {
                btif_a2dp_ack_fail();
                /* pending start flag will be cleared when exit current state */
            }
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;

        case BTA_AV_RECONFIG_EVT:
            if((btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START) &&
                (p_av->reconfig.status == BTA_AV_SUCCESS))
            {
               APPL_TRACE_WARNING("reconfig done BTA_AVstart()");
               BTA_AvStart();
            }
            else if(btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START)
            {
               btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_START;
               btif_a2dp_ack_fail();
            }
            break;

        case BTIF_AV_CONNECT_REQ_EVT:
            if (memcmp ((bt_bdaddr_t*)p_data, &(btif_av_cb.peer_bda),
                sizeof(btif_av_cb.peer_bda)) == 0)
            {
                BTIF_TRACE_DEBUG("%s: Ignore BTIF_AV_CONNECT_REQ_EVT for same device", __func__);
            }
            else
            {
                BTIF_TRACE_DEBUG("%s: Moved to opened by Other Incoming Conn req", __func__);
                btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                        (bt_bdaddr_t*)p_data);
            }
            btif_queue_advance();
            break;

        case BTIF_AV_OFFLOAD_START_REQ_EVT:
            btif_a2dp_on_offload_started(BTA_AV_FAIL);
            BTIF_TRACE_ERROR("BTIF_AV_OFFLOAD_START_REQ_EVT: Stream not Started Opened");
            break;

        CHECK_RC_EVENT(event, p_data);

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                               dump_av_sm_event_name(event));
            return FALSE;

    }
    return TRUE;
}

/*****************************************************************************
**
** Function     btif_av_state_started_handler
**
** Description  Handles AV events while A2DP stream is started
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_started_handler(btif_sm_event_t event, void *p_data)
{
    tBTA_AV *p_av = (tBTA_AV*)p_data;

    BTIF_TRACE_DEBUG("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    switch (event)
    {
#ifdef BLUETOOTH_RTK
        case BTA_AV_RECONFIG_EVT:
        {
            if(p_av->reconfig.status == BTA_AV_SUCCESS)
            {
                BTA_AvStart();
                btif_av_cb.flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;
                btif_report_audio_state(BTAV_AUDIO_STATE_STARTED, &(btif_av_cb.peer_bda));
                adjust_priority_a2dp(TRUE);
            }
        }
        break;
#endif
        case BTIF_SM_ENTER_EVT:

            /* we are again in started state, clear any remote suspend flags */
            btif_av_cb.flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;

            /**
             * Report to components above that we have entered the streaming
             * stage, this should usually be followed by focus grant.
             * see update_audio_focus_state()
             */
            btif_report_audio_state(BTAV_AUDIO_STATE_STARTED, &(btif_av_cb.peer_bda));

            /* increase the a2dp consumer task priority temporarily when start
            ** audio playing, to avoid overflow the audio packet queue. */
            adjust_priority_a2dp(TRUE);

            break;

        case BTIF_SM_EXIT_EVT:
            /* restore the a2dp consumer task priority when stop audio playing. */
            adjust_priority_a2dp(FALSE);

            break;

        case BTIF_AV_START_STREAM_REQ_EVT:
            /* we were remotely started, just ack back the local request */
            if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
                btif_a2dp_on_started(NULL, TRUE);
            break;

        /* fixme -- use suspend = true always to work around issue with BTA AV */
        case BTIF_AV_STOP_STREAM_REQ_EVT:
        case BTIF_AV_SUSPEND_STREAM_REQ_EVT:

            /* set pending flag to ensure btif task is not trying to restart
               stream while suspend is in progress */
            btif_av_cb.flags |= BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;

            /* if we were remotely suspended but suspend locally, local suspend
               always overrides */
            btif_av_cb.flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;

            if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
            {
            /* immediately stop transmission of frames while suspend is pending */
                btif_a2dp_set_tx_flush(TRUE);
            }

            if (btif_av_cb.peer_sep == AVDT_TSEP_SRC) {
                btif_a2dp_set_rx_flush(TRUE);
                btif_a2dp_on_stopped(NULL);
            }

            BTA_AvStop(TRUE);
            break;

        case BTIF_AV_DISCONNECT_REQ_EVT:

            /* request avdtp to close */
            BTA_AvClose(btif_av_cb.bta_handle);
            if (btif_av_cb.peer_sep == AVDT_TSEP_SRC) {
                BTA_AvCloseRc(btif_av_cb.bta_handle);
            }

            /* inform the application that we are disconnecting */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTING, &(btif_av_cb.peer_bda));

            /* wait in closing state until fully closed */
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_CLOSING);
            break;

        case BTA_AV_SUSPEND_EVT:

            BTIF_TRACE_EVENT("BTA_AV_SUSPEND_EVT status %d, init %d",
                 p_av->suspend.status, p_av->suspend.initiator);

            /* a2dp suspended, stop media task until resumed */
            btif_a2dp_on_suspended(&p_av->suspend);

            /* if not successful, remain in current state */
            if (p_av->suspend.status != BTA_AV_SUCCESS)
            {
                btif_av_cb.flags &= ~BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;

               if (btif_av_cb.peer_sep == AVDT_TSEP_SNK)
               {
                /* suspend failed, reset back tx flush state */
                    btif_a2dp_set_tx_flush(FALSE);
               }
                return FALSE;
            }

            if (p_av->suspend.initiator != TRUE)
            {
                /* remote suspend, notify HAL and await audioflinger to
                   suspend/stop stream */

                /* set remote suspend flag to block media task from restarting
                   stream only if we did not already initiate a local suspend */
                if ((btif_av_cb.flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING) == 0)
                    btif_av_cb.flags |= BTIF_AV_FLAG_REMOTE_SUSPEND;

                btif_report_audio_state(BTAV_AUDIO_STATE_REMOTE_SUSPEND, &(btif_av_cb.peer_bda));
            }
            else
            {
                btif_report_audio_state(BTAV_AUDIO_STATE_STOPPED, &(btif_av_cb.peer_bda));
            }

            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_OPENED);

            /* suspend completed and state changed, clear pending status */
            btif_av_cb.flags &= ~BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;
            break;

        case BTA_AV_STOP_EVT:

            btif_av_cb.flags |= BTIF_AV_FLAG_PENDING_STOP;
            btif_a2dp_on_stopped(&p_av->suspend);

            btif_report_audio_state(BTAV_AUDIO_STATE_STOPPED, &(btif_av_cb.peer_bda));

            /* if stop was successful, change state to open */
            if (p_av->suspend.status == BTA_AV_SUCCESS)
                btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_OPENED);

            break;

        case BTA_AV_CLOSE_EVT:

             btif_av_cb.flags |= BTIF_AV_FLAG_PENDING_STOP;

            /* avdtp link is closed */
            btif_a2dp_on_stopped(NULL);

            /* inform the application that we are disconnected */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));

            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;

        case BTIF_AV_OFFLOAD_START_REQ_EVT:
            BTA_AvOffloadStart(btif_av_cb.bta_handle);
            break;

        case BTA_AV_OFFLOAD_START_RSP_EVT:

            btif_a2dp_on_offload_started(p_av->status);
            break;

        CHECK_RC_EVENT(event, p_data);

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                                 dump_av_sm_event_name(event));
            return FALSE;

    }
    return TRUE;
}

/*****************************************************************************
**  Local event handlers
******************************************************************************/

static void btif_av_handle_event(UINT16 event, char* p_param)
{
    switch(event)
    {
        case BTIF_AV_CLEANUP_REQ_EVT:
            BTIF_TRACE_EVENT("%s: BTIF_AV_CLEANUP_REQ_EVT", __FUNCTION__);
            btif_a2dp_stop_media_task();
            break;

        default:
            btif_sm_dispatch(btif_av_cb.sm_handle, event, (void*)p_param);
            btif_av_event_free_data(event, p_param);
    }
}

void btif_av_event_deep_copy(UINT16 event, char *p_dest, char *p_src)
{
    tBTA_AV *av_src = (tBTA_AV *)p_src;
    tBTA_AV *av_dest = (tBTA_AV *)p_dest;

    // First copy the structure
    maybe_non_aligned_memcpy(av_dest, av_src, sizeof(*av_src));

    switch (event)
    {
        case BTA_AV_META_MSG_EVT:
            if (av_src->meta_msg.p_data && av_src->meta_msg.len)
            {
                av_dest->meta_msg.p_data = osi_calloc(av_src->meta_msg.len);
                memcpy(av_dest->meta_msg.p_data, av_src->meta_msg.p_data,
                       av_src->meta_msg.len);
            }

            if (av_src->meta_msg.p_msg)
            {
                av_dest->meta_msg.p_msg = osi_calloc(sizeof(tAVRC_MSG));
                memcpy(av_dest->meta_msg.p_msg, av_src->meta_msg.p_msg,
                       sizeof(tAVRC_MSG));

                if (av_src->meta_msg.p_msg->vendor.p_vendor_data &&
                    av_src->meta_msg.p_msg->vendor.vendor_len)
                {
                    av_dest->meta_msg.p_msg->vendor.p_vendor_data = osi_calloc(
                        av_src->meta_msg.p_msg->vendor.vendor_len);
                    memcpy(av_dest->meta_msg.p_msg->vendor.p_vendor_data,
                        av_src->meta_msg.p_msg->vendor.p_vendor_data,
                        av_src->meta_msg.p_msg->vendor.vendor_len);
                }
            }
            break;

        default:
            break;
    }
}

static void btif_av_event_free_data(btif_sm_event_t event, void *p_data)
{
    switch (event)
    {
        case BTA_AV_META_MSG_EVT:
            {
                tBTA_AV *av = (tBTA_AV *)p_data;
                osi_free_and_reset((void **)&av->meta_msg.p_data);

                if (av->meta_msg.p_msg) {
                    osi_free(av->meta_msg.p_msg->vendor.p_vendor_data);
                    osi_free_and_reset((void **)&av->meta_msg.p_msg);
                }
            }
            break;

        default:
            break;
    }
}

static void bte_av_callback(tBTA_AV_EVT event, tBTA_AV *p_data)
{
    btif_transfer_context(btif_av_handle_event, event,
                          (char*)p_data, sizeof(tBTA_AV), btif_av_event_deep_copy);
}

static void bte_av_media_callback(tBTA_AV_EVT event, tBTA_AV_MEDIA *p_data)
{
    btif_sm_state_t state;
    UINT8 que_len;
    tA2D_STATUS a2d_status;
    tA2D_SBC_CIE sbc_cie;
    btif_av_sink_config_req_t config_req;

    if (event == BTA_AV_MEDIA_DATA_EVT)/* Switch to BTIF_MEDIA context */
    {
        state= btif_sm_get_state(btif_av_cb.sm_handle);
        if ( (state == BTIF_AV_STATE_STARTED) || /* send SBC packets only in Started State */
             (state == BTIF_AV_STATE_OPENED) )
        {
            que_len = btif_media_sink_enque_buf((BT_HDR *)p_data);
            BTIF_TRACE_DEBUG(" Packets in Que %d",que_len);
        }
        else
            return;
    }

    if (event == BTA_AV_MEDIA_SINK_CFG_EVT) {
        /* send a command to BT Media Task */
        btif_reset_decoder((UINT8*)(p_data->avk_config.codec_info));
        a2d_status = A2D_ParsSbcInfo(&sbc_cie, (UINT8 *)(p_data->avk_config.codec_info), FALSE);
        if (a2d_status == A2D_SUCCESS) {
            /* Switch to BTIF context */
            config_req.sample_rate = btif_a2dp_get_track_frequency(sbc_cie.samp_freq);
            config_req.channel_count = btif_a2dp_get_track_channel_count(sbc_cie.ch_mode);
            memcpy(&config_req.peer_bd,(UINT8*)(p_data->avk_config.bd_addr),
                                                              sizeof(config_req.peer_bd));
            btif_transfer_context(btif_av_handle_event, BTIF_AV_SINK_CONFIG_REQ_EVT,
                                     (char*)&config_req, sizeof(config_req), NULL);
        } else {
            APPL_TRACE_ERROR("ERROR dump_codec_info A2D_ParsSbcInfo fail:%d", a2d_status);
        }
    }
}
/*******************************************************************************
**
** Function         btif_av_init
**
** Description      Initializes btif AV if not already done
**
** Returns          bt_status_t
**
*******************************************************************************/

bt_status_t btif_av_init(int service_id)
{
    if (btif_av_cb.sm_handle == NULL)
    {
        alarm_free(av_open_on_rc_timer);
        av_open_on_rc_timer = alarm_new("btif_av.av_open_on_rc_timer");
        if (!btif_a2dp_start_media_task())
            return BT_STATUS_FAIL;

                btif_enable_service(BTA_A2DP_SOURCE_SERVICE_ID);
#if (BTA_AV_SINK_INCLUDED == TRUE)
        btif_enable_service(BTA_A2DP_SINK_SERVICE_ID);
#endif

        /* Also initialize the AV state machine */
        btif_av_cb.sm_handle =
                btif_sm_init((const btif_sm_handler_t*)btif_av_state_handlers, BTIF_AV_STATE_IDLE);

        btif_a2dp_on_init();
    }

    return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         init_src
**
** Description      Initializes the AV interface for source mode
**
** Returns          bt_status_t
**
*******************************************************************************/

static bt_status_t init_src(btav_callbacks_t* callbacks)
{
    BTIF_TRACE_EVENT("%s()", __func__);

    bt_status_t status = btif_av_init(BTA_A2DP_SOURCE_SERVICE_ID);
    if (status == BT_STATUS_SUCCESS)
        bt_av_src_callbacks = callbacks;

    return status;
}

/*******************************************************************************
**
** Function         init_sink
**
** Description      Initializes the AV interface for sink mode
**
** Returns          bt_status_t
**
*******************************************************************************/

static bt_status_t init_sink(btav_callbacks_t* callbacks)
{
    BTIF_TRACE_EVENT("%s()", __func__);

    bt_status_t status = btif_av_init(BTA_A2DP_SINK_SERVICE_ID);
    if (status == BT_STATUS_SUCCESS)
        bt_av_sink_callbacks = callbacks;
#ifdef BLUETOOTH_RTK
#if (BTA_AV_SINK_INCLUDED == TRUE)
        //btif_enable_service(BTA_A2DP_SINK_SERVICE_ID);
#endif
#endif

    return status;
}

#ifdef USE_AUDIO_TRACK
/*******************************************************************************
**
** Function         update_audio_focus_state
**
** Description      Updates the final focus state reported by components calling
**                  this module.
**
** Returns          None
**
*******************************************************************************/
void update_audio_focus_state(int state)
{
    BTIF_TRACE_DEBUG("%s state %d ",__func__, state);
    btif_a2dp_set_audio_focus_state(state);
}

/*******************************************************************************
**
** Function         update_audio_track_gain
**
** Description      Updates the track gain (used for ducking).
**
** Returns          None
**
*******************************************************************************/
void update_audio_track_gain(float gain)
{
    BTIF_TRACE_DEBUG("%s gain %f ",__func__, gain);
    btif_a2dp_set_audio_track_gain(gain);
}
#endif

/*******************************************************************************
**
** Function         connect
**
** Description      Establishes the AV signalling channel with the remote headset
**
** Returns          bt_status_t
**
*******************************************************************************/

static bt_status_t connect_int(bt_bdaddr_t *bd_addr, uint16_t uuid)
{
    btif_av_connect_req_t connect_req;
    connect_req.target_bda = bd_addr;
    connect_req.uuid = uuid;
    BTIF_TRACE_EVENT("%s", __FUNCTION__);

#ifdef BLUETOOTH_RTK
    btif_av_cb.uuid = uuid;
#endif
    btif_sm_dispatch(btif_av_cb.sm_handle, BTIF_AV_CONNECT_REQ_EVT, (char*)&connect_req);

    return BT_STATUS_SUCCESS;
}

static bt_status_t src_connect_sink(bt_bdaddr_t *bd_addr)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    CHECK_BTAV_INIT();

    return btif_queue_connect(UUID_SERVCLASS_AUDIO_SOURCE, bd_addr, connect_int);
}

static bt_status_t sink_connect_src(bt_bdaddr_t *bd_addr)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    CHECK_BTAV_INIT();

    return btif_queue_connect(UUID_SERVCLASS_AUDIO_SINK, bd_addr, connect_int);
}

/*******************************************************************************
**
** Function         disconnect
**
** Description      Tears down the AV signalling channel with the remote headset
**
** Returns          bt_status_t
**
*******************************************************************************/
static bt_status_t disconnect(bt_bdaddr_t *bd_addr)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);

    CHECK_BTAV_INIT();

    /* Switch to BTIF context */
    return btif_transfer_context(btif_av_handle_event, BTIF_AV_DISCONNECT_REQ_EVT,
                                 (char*)bd_addr, sizeof(bt_bdaddr_t), NULL);
}

/*******************************************************************************
**
** Function         cleanup
**
** Description      Shuts down the AV interface and does the cleanup
**
** Returns          None
**
*******************************************************************************/
static void cleanup(int service_uuid)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);

    btif_transfer_context(btif_av_handle_event, BTIF_AV_CLEANUP_REQ_EVT, NULL, 0, NULL);

    btif_disable_service(service_uuid);

    /* Also shut down the AV state machine */
    btif_sm_shutdown(btif_av_cb.sm_handle);
    btif_av_cb.sm_handle = NULL;
#ifdef BLUETOOTH_RTK
    btif_av_cb.peer_sep = 0;
    btif_av_cb.uuid = 0;
#endif
}

static void cleanup_src(void) {
    BTIF_TRACE_EVENT("%s", __FUNCTION__);

    if (bt_av_src_callbacks)
    {
        bt_av_src_callbacks = NULL;
        if (bt_av_sink_callbacks == NULL)
            cleanup(BTA_A2DP_SOURCE_SERVICE_ID);
    }
}

static void cleanup_sink(void) {
    BTIF_TRACE_EVENT("%s", __FUNCTION__);

    if (bt_av_sink_callbacks)
    {
        bt_av_sink_callbacks = NULL;
        if (bt_av_src_callbacks == NULL)
            cleanup(BTA_A2DP_SINK_SERVICE_ID);
    }
}
#ifdef BLUETOOTH_RTK
static const btav_interface_t bt_av_src_interface = {
    .size = sizeof(btav_interface_t),
    .init = init_src,
    .connect = src_connect_sink,
    .disconnect = disconnect,
    .cleanup = cleanup_src,
    .set_audio_focus_state =  NULL,
    .set_audio_track_gain = NULL,
};
#else
static const btav_interface_t bt_av_src_interface = {
    sizeof(btav_interface_t),
    init_src,
    src_connect_sink,
    disconnect,
    cleanup_src,
    NULL,
    NULL,
};
#endif
#ifdef BLUETOOTH_RTK
static const btav_interface_t bt_av_sink_interface = {
    .size = sizeof(btav_interface_t),
    .init = init_sink,
    .connect = sink_connect_src,
    .disconnect = disconnect,
    .cleanup = cleanup_sink,
#ifdef USE_AUDIO_TRACK
    .set_audio_focus_state = update_audio_focus_state,
    .set_audio_track_gain = update_audio_track_gain,
#else
    .set_audio_focus_state = NULL,
    .set_audio_track_gain = NULL,
#endif
};
#else
static const btav_interface_t bt_av_sink_interface = {
    sizeof(btav_interface_t),
    init_sink,
    sink_connect_src,
    disconnect,
    cleanup_sink,
#ifdef USE_AUDIO_TRACK
    update_audio_focus_state,
    update_audio_track_gain,
#else
    NULL,
    NULL,
#endif
};
#endif
/*******************************************************************************
**
** Function         btif_av_get_sm_handle
**
** Description      Fetches current av SM handle
**
** Returns          None
**
*******************************************************************************/

btif_sm_handle_t btif_av_get_sm_handle(void)
{
    return btif_av_cb.sm_handle;
}

/*******************************************************************************
**
** Function         btif_av_get_addr
**
** Description      Fetches current AV BD address
**
** Returns          BD address
**
*******************************************************************************/

bt_bdaddr_t btif_av_get_addr(void)
{
    return btif_av_cb.peer_bda;
}

/*******************************************************************************
** Function         btif_av_is_sink_enabled
**
** Description      Checks if A2DP Sink is enabled or not
**
** Returns          TRUE if A2DP Sink is enabled, false otherwise
**
*******************************************************************************/

BOOLEAN btif_av_is_sink_enabled(void)
{
    return (bt_av_sink_callbacks != NULL) ? TRUE : FALSE;
}

/*******************************************************************************
**
** Function         btif_av_stream_ready
**
** Description      Checks whether AV is ready for starting a stream
**
** Returns          None
**
*******************************************************************************/

BOOLEAN btif_av_stream_ready(void)
{
    btif_sm_state_t state = btif_sm_get_state(btif_av_cb.sm_handle);

    BTIF_TRACE_DEBUG("btif_av_stream_ready : sm hdl %d, state %d, flags %x",
                btif_av_cb.sm_handle, state, btif_av_cb.flags);

    /* also make sure main adapter is enabled */
    if (btif_is_enabled() == 0)
    {
        BTIF_TRACE_EVENT("main adapter not enabled");
        return FALSE;
    }

    /* check if we are remotely suspended or stop is pending */
    if (btif_av_cb.flags & (BTIF_AV_FLAG_REMOTE_SUSPEND|BTIF_AV_FLAG_PENDING_STOP))
        return FALSE;

    return (state == BTIF_AV_STATE_OPENED);
}

/*******************************************************************************
**
** Function         btif_av_stream_started_ready
**
** Description      Checks whether AV ready for media start in streaming state
**
** Returns          None
**
*******************************************************************************/

BOOLEAN btif_av_stream_started_ready(void)
{
    btif_sm_state_t state = btif_sm_get_state(btif_av_cb.sm_handle);

    BTIF_TRACE_DEBUG("btif_av_stream_started : sm hdl %d, state %d, flags %x",
                btif_av_cb.sm_handle, state, btif_av_cb.flags);

    /* disallow media task to start if we have pending actions */
    if (btif_av_cb.flags & (BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING | BTIF_AV_FLAG_REMOTE_SUSPEND
        | BTIF_AV_FLAG_PENDING_STOP))
        return FALSE;

    return (state == BTIF_AV_STATE_STARTED);
}

/*******************************************************************************
**
** Function         btif_dispatch_sm_event
**
** Description      Send event to AV statemachine
**
** Returns          None
**
*******************************************************************************/

/* used to pass events to AV statemachine from other tasks */
void btif_dispatch_sm_event(btif_av_sm_event_t event, void *p_data, int len)
{
    /* Switch to BTIF context */
    btif_transfer_context(btif_av_handle_event, event,
                          (char*)p_data, len, NULL);
}

/*******************************************************************************
**
** Function         btif_av_execute_service
**
** Description      Initializes/Shuts down the service
**
** Returns          BT_STATUS_SUCCESS on success, BT_STATUS_FAIL otherwise
**
*******************************************************************************/
bt_status_t btif_av_execute_service(BOOLEAN b_enable)
{
     if (b_enable)
     {
         /* TODO: Removed BTA_SEC_AUTHORIZE since the Java/App does not
          * handle this request in order to allow incoming connections to succeed.
          * We need to put this back once support for this is added */

         /* Added BTA_AV_FEAT_NO_SCO_SSPD - this ensures that the BTA does not
          * auto-suspend av streaming on AG events(SCO or Call). The suspend shall
          * be initiated by the app/audioflinger layers */
         /* Support for browsing for SDP record should work only if we enable BROWSE
          * while registering. */
#if (AVRC_METADATA_INCLUDED == TRUE)
         BTA_AvEnable(BTA_SEC_AUTHENTICATE,
             BTA_AV_FEAT_RCTG|BTA_AV_FEAT_METADATA|BTA_AV_FEAT_VENDOR|BTA_AV_FEAT_NO_SCO_SSPD
#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
             |BTA_AV_FEAT_RCCT
             |BTA_AV_FEAT_ADV_CTRL
#endif
             ,bte_av_callback);
#else
         BTA_AvEnable(BTA_SEC_AUTHENTICATE, (BTA_AV_FEAT_RCTG | BTA_AV_FEAT_NO_SCO_SSPD),
                      bte_av_callback);
#endif
         BTA_AvRegister(BTA_AV_CHNL_AUDIO, BTIF_AV_SERVICE_NAME, 0, bte_av_media_callback,
                                                             UUID_SERVCLASS_AUDIO_SOURCE);
     }
     else {
         BTA_AvDeregister(btif_av_cb.bta_handle);
         BTA_AvDisable();
     }
     return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         btif_av_sink_execute_service
**
** Description      Initializes/Shuts down the service
**
** Returns          BT_STATUS_SUCCESS on success, BT_STATUS_FAIL otherwise
**
*******************************************************************************/
bt_status_t btif_av_sink_execute_service(BOOLEAN b_enable)
{
     /*if (b_enable)
     {

         BTA_AvEnable(BTA_SEC_AUTHENTICATE, BTA_AV_FEAT_NO_SCO_SSPD|BTA_AV_FEAT_RCCT|
                                            BTA_AV_FEAT_METADATA|BTA_AV_FEAT_VENDOR|
                                            BTA_AV_FEAT_ADV_CTRL|BTA_AV_FEAT_RCTG,
                                                                        bte_av_callback);
         BTA_AvRegister(BTA_AV_CHNL_AUDIO, BTIF_AVK_SERVICE_NAME, 0, bte_av_media_callback,
                                                                UUID_SERVCLASS_AUDIO_SINK);
     }
     else {
         BTA_AvDeregister(btif_av_cb.bta_handle);
         BTA_AvDisable();
     }*/

    BTA_AvEnable_Sink(b_enable);

     return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         btif_av_get_src_interface
**
** Description      Get the AV callback interface for A2DP source profile
**
** Returns          btav_interface_t
**
*******************************************************************************/
const btav_interface_t *btif_av_get_src_interface(void)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    return &bt_av_src_interface;
}

/*******************************************************************************
**
** Function         btif_av_get_sink_interface
**
** Description      Get the AV callback interface for A2DP sink profile
**
** Returns          btav_interface_t
**
*******************************************************************************/
const btav_interface_t *btif_av_get_sink_interface(void)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    return &bt_av_sink_interface;
}

/*******************************************************************************
**
** Function         btif_av_is_connected
**
** Description      Checks if av has a connected sink
**
** Returns          BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_av_is_connected(void)
{
    btif_sm_state_t state = btif_sm_get_state(btif_av_cb.sm_handle);
    return ((state == BTIF_AV_STATE_OPENED) || (state ==  BTIF_AV_STATE_STARTED));
}

/*******************************************************************************
**
** Function         btif_av_is_peer_edr
**
** Description      Check if the connected a2dp device supports
**                  EDR or not. Only when connected this function
**                  will accurately provide a true capability of
**                  remote peer. If not connected it will always be false.
**
** Returns          TRUE if remote device is capable of EDR
**
*******************************************************************************/
BOOLEAN btif_av_is_peer_edr(void)
{
    ASSERTC(btif_av_is_connected(), "No active a2dp connection", 0);

    if (btif_av_cb.edr)
        return TRUE;
    else
        return FALSE;
}

/******************************************************************************
**
** Function        btif_av_clear_remote_suspend_flag
**
** Description     Clears btif_av_cd.flags if BTIF_AV_FLAG_REMOTE_SUSPEND is set
**
** Returns          void
******************************************************************************/
void btif_av_clear_remote_suspend_flag(void)
{
    BTIF_TRACE_DEBUG("%s: flag :%x",__func__, btif_av_cb.flags);
    btif_av_cb.flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;
}

/*******************************************************************************
**
** Function         btif_av_peer_supports_3mbps
**
** Description      Check if the connected A2DP device supports
**                  3 Mbps EDR. This function only works if connected.
**                  If not connected it will always be false.
**
** Returns          TRUE if remote device is EDR and supports 3 Mbps
**
*******************************************************************************/
BOOLEAN btif_av_peer_supports_3mbps(void)
{
    BOOLEAN is3mbps = ((btif_av_cb.edr & BTA_AV_EDR_3MBPS) != 0);
    BTIF_TRACE_DEBUG("%s: connected %d, edr_3mbps %d", __func__,
            btif_av_is_connected(), is3mbps);
    return (btif_av_is_connected() && is3mbps);
}
#ifdef BLUETOOTH_RTK
/******************************************************************************
**
** Function        btif_av_get_peer_role
**
** Description     true: peer is sink; false: peer is source
**
** Returns          BOOLEAN
******************************************************************************/
static BOOLEAN btif_av_get_peer_role(bt_bdaddr_t *bd_addr)
{
    bool peerIsSource = false;
    bt_property_t remote_properties;
    char uuid_a2dp[128] = "0000110b-0000-1000-8000-00805f9b34fb";
    uuid_string_t *uuid_string = uuid_string_new();
    bt_uuid_t remote_uuids[BT_MAX_NUM_UUIDS];
    BTIF_STORAGE_FILL_PROPERTY(&remote_properties, BT_PROPERTY_UUIDS,sizeof(remote_uuids), remote_uuids);
    btif_storage_get_remote_device_property(bd_addr,&remote_properties);
    int i = 0;
    for(i=0;i < remote_properties.len;i++){
         uuid_to_string(&remote_uuids[i],uuid_string);
         if(strcmp(uuid_a2dp,uuid_string_data(uuid_string)) == 0){
              peerIsSource = true;
              break;
          }
     }
    uuid_string_free(uuid_string);
    return peerIsSource;
}
#endif
