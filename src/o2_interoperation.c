//
//  o2_interoperation.c
//  o2
//
//  Created by ĺźĺź  on 3/31/16.
//
/* Design notes:
 *    We handle incoming OSC ports using 
 * o2_osc_port_new(service_name, port_num, tcp_flag), which puts an
 * entry in the fds_info table that says incoming OSC messages are 
 * handled by the service_name (which may or may not be local). Thus, 
 * when an OSC message arrives, we use the incoming data to construct 
 * a full O2 message. We can use the OSC message length plus the 
 * service name length plus a timestamp length (plus some padding) to 
 * determine how much message space to allocate. Then, we can receive
 * the data with some offset allowing us to prepend the timestamp and
 * service name. Finally, we just send the message, resulting in
 * either a local dispatch or forwarding to an O2 service.
 *
 *   We handle outgoing OSC messages using
 * o2_osc_delegate(service_name, ip, port_num), which puts an
 * entry in the top-level hash table with the OSC socket.
 */

#include "o2.h"
#include "o2_internal.h"
#include "o2_message.h"
#include "o2_sched.h"
#include "o2_send.h"
#include "o2_interoperation.h"

#include "errno.h"

static o2_message_ptr osc_to_o2(int32_t len, char *oscmsg, o2string service);

static uint64_t osc_time_offset = 0;

uint64_t o2_osc_time_offset(uint64_t offset)
{
    uint64_t old = osc_time_offset;
    osc_time_offset = offset;
    return old;
}

#define TWO32 4294967296.0


o2_time o2_time_from_osc(uint64_t osctime)
{
#if IS_LITTLE_ENDIAN
    osctime = swap64(osctime); // message is byte swapped
#endif
    osctime -= osc_time_offset;
    return osctime / TWO32;
}


uint64_t o2_time_to_osc(o2_time o2time)
{
    uint64_t osctime = (uint64_t) (o2time * TWO32);
    return osctime + osc_time_offset;
}


/* create a port to receive OSC messages.
 * Messages are directed to service_name.
 * The service is not created by this call, but if the service
 * does not exist when an OSC message arrives, the message will be
 * dropped.
 *
 * Algorithm: Add a socket, put service name in info
 */
int o2_osc_port_new(const char *service_name, int port_num, int tcp_flag)
{
    process_info_ptr info;
    if (tcp_flag) {
        RETURN_IF_ERROR(o2_make_tcp_recv_socket(OSC_TCP_SERVER_SOCKET, port_num,
                                                &o2_osc_tcp_accept_handler, &info));
    } else {
        RETURN_IF_ERROR(o2_make_udp_recv_socket(OSC_SOCKET, &port_num, &info));
    }
    info->osc.service_name = o2_heapify(service_name);
    return O2_SUCCESS;
}


int o2_osc_port_free(int port_num)
{
    int result = O2_FAIL;
    o2string service_name_copy = NULL;
    for (int i = 0; i < o2_context->fds_info.length; i++) {
        process_info_ptr info = GET_PROCESS(i);
        if ((info->tag == OSC_TCP_SERVER_SOCKET ||
             info->tag == OSC_TCP_SOCKET ||
             info->tag == OSC_SOCKET) &&
            info->port == port_num) {
            // we need to delete the osc_service_name, but it is shared
            // by any OSC_TCP_SOCKET record, and it seems wrong for them
            // to get a dangling pointer, so we'll just remember the
            // string and free it after all the o2_context->fds_info records are
            // removed.
            if (info->osc.service_name) {
                assert(service_name_copy == NULL || service_name_copy == info->osc.service_name);
                service_name_copy = info->osc.service_name;
                info->osc.service_name = NULL;
            }
            o2_socket_mark_to_free(info);
            result = O2_SUCCESS; // actual found and removed a port
        }
    }
    if (service_name_copy) O2_FREE((void *)service_name_copy);
    return O2_SUCCESS;
}


// messages to this service are forwarded as OSC messages
// does the service exist as a local service? If so, fail.
// make an osc_info record for this delegation of service
// if tcp_flag, make a tcp connection with tag OSC_TCP_SOCKET
// and set the tcp_socket_info to the process_info_ptr you get
// for the tcp connection.
// if udp, then set the udp address info in the osc_info
// add osc_info as the service
int o2_osc_delegate(const char *service_name, const char *ip, int port_num, int tcp_flag)
{
    if (!o2_application_name) {
        return O2_NOT_INITIALIZED;
    }
    if (!service_name || strchr(service_name, '/'))
        return O2_BAD_SERVICE_NAME;
    osc_info_ptr osc = (osc_info_ptr) O2_MALLOC(sizeof(osc_info));
    osc->tag = OSC_REMOTE_SERVICE;
    char padded_name[NAME_BUF_LEN];
    o2_string_pad(padded_name, service_name);
    int rslt = o2_service_provider_new(padded_name, (o2_info_ptr) osc, o2_context->process, "");
    if (rslt != O2_SUCCESS) {
        O2_FREE(osc);
        return rslt;
    }
    rslt = O2_SUCCESS;
    if (streql(ip, "")) ip = "localhost";
    char port[24]; // can't overrun even with 64-bit int
    sprintf(port, "%d", port_num);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = PF_INET;
    struct addrinfo *aiptr = NULL;
    struct sockaddr_in remote_addr;

    osc->port = port_num;
    if (tcp_flag) {
        process_info_ptr info;
        RETURN_IF_ERROR(o2_make_tcp_recv_socket(
                OSC_TCP_CLIENT, 0, &o2_osc_delegate_handler, &info));
        // make the connection
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        if (getaddrinfo(ip, port, &hints, &aiptr)) {
            goto hostname_to_netaddr_fail;
        }
        memcpy(&remote_addr, aiptr->ai_addr, sizeof(remote_addr));
        remote_addr.sin_port = htons((short) port_num);
        SOCKET sock = DA_LAST(o2_context->fds, struct pollfd)->fd;
        osc->tcp_socket_info = info;
        if (connect(sock, (struct sockaddr *) &remote_addr,
                    sizeof(remote_addr)) == -1) {
            perror("OSC Server connect error!");
            o2_context->fds_info.length--;
            o2_context->fds.length--;
            rslt = O2_TCP_CONNECT_FAIL;
            O2_FREE(info);
            goto fail_and_exit;
        }
        info->osc.service_name = o2_heapify(service_name);
        o2_disable_sigpipe(sock);
    } else {
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        if (getaddrinfo(ip, port, &hints, &aiptr)) {
            goto hostname_to_netaddr_fail;
        }
        memcpy(&remote_addr, aiptr->ai_addr, sizeof(remote_addr));
        if (remote_addr.sin_port == 0) {
            remote_addr.sin_port = htons((short) port_num);
        }
        memcpy(&osc->udp_sa, &remote_addr, sizeof(remote_addr));
        osc->tcp_socket_info = NULL;
    }
    rslt = O2_SUCCESS;
    goto just_exit;
  hostname_to_netaddr_fail:
    rslt = O2_HOSTNAME_TO_NETADDR_FAIL;
  fail_and_exit:
    O2_FREE(osc);
  just_exit:
    if (aiptr) freeaddrinfo(aiptr);
    return rslt;
}


static o2_message_ptr osc_bundle_to_o2(int32_t len, char *oscmsg, o2string service)
{
    // osc bundle has the form #bundle, timestamp, messages
    // It is assumed that all embedded messages in an OSC bundle
    // are destined for the same service (info->osc_service_name).
    // Bundle translation is going to unpack and repack the embedded
    // messages: not the most efficient, but simpler.
    
    o2_time ts = o2_time_from_osc(*((uint64_t *) (oscmsg + 8)));
    char *end_of_msg = oscmsg + len;
    char *embedded = oscmsg + 20; // skip #bundle & timestamp & length
    o2_message_ptr o2msg = NULL;
    o2_message_ptr msg_list = NULL;
    o2_message_ptr last = NULL;
    while (embedded < end_of_msg) {
        int embedded_len = ((int32_t *) embedded)[-1];
#if IS_LITTLE_ENDIAN
        embedded_len = swap32(embedded_len);
#endif
        if (PTR(embedded) + embedded_len <= end_of_msg) {
            o2msg = osc_to_o2(embedded_len, embedded, service);
        }
        if (!o2msg) {
            o2_message_list_free(msg_list);
            return NULL;
        }
        o2msg->next = NULL; // make sure link is initialized
        // remember embedded messages on list
        if (last == NULL) { // first element goes at head of list
            msg_list = o2msg;
        } else {
            last->next = o2msg;
        }
        last = o2msg;

        embedded += embedded_len + sizeof(int32_t);
    }
    // add each element to a message
    o2_send_start();
    while (msg_list) {
        o2_message_ptr next = msg_list->next;
        o2_add_message(msg_list);
        o2_message_free(msg_list);
        msg_list = next;
    }
    return o2_service_message_finish(ts, service, "", TRUE);
}


// convert an osc message in network byte order to o2 message in host order
static o2_message_ptr osc_to_o2(int32_t len, char *oscmsg, o2string service)
{
    // osc message has the form: address, types, data
    // o2 message has the form: timestamp, address, types, data
    // o2 address must have a info->u.osc_service_name prefix
    // since we need more space, allocate a new message for o2 and
    // copy the data to it


    if (strcmp(oscmsg, "#bundle") == 0) { // it's a bundle
        return osc_bundle_to_o2(len, oscmsg, service);
    } else { // normal message
        int service_len = (int) strlen(service);
        // length in data part will be timestamp + slash (1) + service name +
        //    o2 data; add another 7 bytes for padding after address
        int o2len = sizeof(double) + 8 + service_len + len;
        o2_message_ptr o2msg = o2_alloc_size_message(o2len);
        o2msg->data.timestamp = 0.0;  // deliver immediately
        o2msg->data.address[0] = '/'; // slash before service name
        strcpy(o2msg->data.address + 1, service);
        // how many bytes in OSC address?
        int addr_len = (int) strlen(oscmsg);
        // compute address of byte after the O2 address string
        char *o2_ptr = o2msg->data.address + 1 + service_len;
        // zero fill to word boundary
        int32_t *fill_ptr = (int32_t *) WORD_ALIGN_PTR(o2_ptr + addr_len);
        *fill_ptr = 0;
        // copy in OSC address string, possibly overwriting some of the fill
        memcpy(o2_ptr, oscmsg, addr_len);
        o2_ptr = PTR(fill_ptr + 1); // get location after O2 address
        // copy type string and OSC message data
        char *osc_ptr = WORD_ALIGN_PTR(oscmsg + addr_len + 4);
        o2len = (int) (oscmsg + len - osc_ptr); // how much payload to copy
        memcpy(o2_ptr, osc_ptr, o2len);
        o2msg->length = (int32_t) (o2_ptr + o2len - PTR(&(o2msg->data)));
#if IS_LITTLE_ENDIAN
        o2_msg_swap_endian(&(o2msg->data), FALSE);
#endif
        return o2msg;
    }
}


// forward an OSC message to an O2 service
int o2_deliver_osc(process_info_ptr info)
{
    char *msg_data = (char *) &(info->message->data); // OSC address starts here
    O2_DBO(printf("%s os_deliver_osc got OSC message %s length %d for service %s\n",
                  o2_debug_prefix, msg_data, info->message->length, info->osc.service_name));
    o2_message_ptr o2msg = osc_to_o2(info->message->length, msg_data,
                                     info->osc.service_name);
    o2_message_free(info->message);
    if (!o2msg) {
        return O2_FAIL;
    }
    // if this came by UDP, tag is OSC_SOCKET, and tcp_flag should be false
    o2msg->tcp_flag = (info->tag != OSC_SOCKET);
    if (o2_message_send_sched(o2msg, TRUE)) { // failure to deliver message will NOT
            // cause the connection to be closed; only the current message
            // will be dropped
        O2_DBO(printf("%s os_deliver_osc: message %s forward to %s failed\n",
                      o2_debug_prefix, msg_data, info->osc.service_name));
    }
    return O2_SUCCESS;
}


// convert O2 message to OSC message which is appended to msg_data.array
// for liblo compatibility, timestamps of embedded bundles are at least
// as late as the containing, or parent, bundle's timestamp.
//
static int msg_data_to_osc_data(osc_info_ptr service, o2_msg_data_ptr msg,
                                o2_time min_time)
{
    // build new message in msg_data
    if (IS_BUNDLE(msg)) {
        if (msg->timestamp > min_time) min_time = msg->timestamp;
        o2_add_bundle_head(o2_time_to_osc(min_time));
        char *end_of_msg = PTR(msg) + MSG_DATA_LENGTH(msg);
        o2_msg_data_ptr embedded = (o2_msg_data_ptr)
            ((msg)->address + o2_strsize((msg)->address) + sizeof(int32_t));
        while (PTR(embedded) < end_of_msg) { int32_t len;
            int32_t *len_ptr = o2_msg_len_ptr();
            len = MSG_DATA_LENGTH(embedded);
            if ((PTR(embedded) + len > end_of_msg) ||
                msg_data_to_osc_data(service, embedded, min_time) != O2_SUCCESS) {
                return O2_FAIL;
            }
            o2_set_msg_length(len_ptr);
            embedded = (o2_msg_data_ptr) (PTR(embedded) + len + sizeof(int32_t));
        }
    } else {
        // Begin by converting to network byte order:
#if IS_LITTLE_ENDIAN
        RETURN_IF_ERROR(o2_msg_swap_endian(msg, TRUE));
#endif
        // Copy address, eliminating service name prefix
        int service_len = (int) strlen(service->service_name) + 1; // include slash
        o2_add_string_or_symbol('s', msg->address + service_len);
        // Get the address of the rest of the message:
        char *types_ptr = msg->address + 4;
        while (types_ptr[-1]) types_ptr += 4;
        o2_add_raw_bytes((int32_t) (PTR(msg) + MSG_DATA_LENGTH(msg) -
                                    types_ptr), types_ptr);
    }
    return O2_SUCCESS;
}


// forward an O2 message to an OSC server
int o2_send_osc(osc_info_ptr service, o2_msg_data_ptr msg, services_entry_ptr services)
{
    o2_send_start();
    RETURN_IF_ERROR(msg_data_to_osc_data(service, msg, 0.0));
    int32_t osc_len;
    char *osc_msg = o2_msg_data_get(&osc_len);
    O2_DBO(printf("%s o2_send_osc sending OSC message %s length %d as "
                  "service %s\n",
                  o2_debug_prefix, osc_msg, osc_len, service->service_name));
    O2_DBO(o2_dbg_msg("original O2 msg is", msg, NULL, NULL));
    // Now we have an OSC message at msg->address. Send it.
    if (service->tcp_socket_info == NULL) { // must be UDP
        if (sendto(local_send_sock, osc_msg, osc_len,
                   0, (struct sockaddr *) &(service->udp_sa),
                   sizeof(service->udp_sa)) < 0) {
            perror("o2_send_osc");
            return O2_SEND_FAIL;
        }
    } else { // send by TCP
        SOCKET fd = DA_GET(o2_context->fds, struct pollfd, service->tcp_socket_info->fds_index)->fd;
        // send length
        int32_t len = htonl(osc_len);
        while (send(fd, (char *) &len, sizeof(int32_t), MSG_NOSIGNAL) < 0) {
            perror("o2_send_osc writing length");
            if (errno != EAGAIN && errno != EINTR)
                goto close_socket;
        }
        // send message body
        while (send(fd, osc_msg, osc_len, MSG_NOSIGNAL) < 0) {
            perror("o2_send_osc writing data");
            if (errno != EAGAIN && errno != EINTR)
                goto close_socket;
        }
    }
    // if there are tappers, send the message to them as well
    int tapper_index = 1; // first tapper will be here
    while (tapper_index < services->services.length) {
        tapper_entry_ptr tapper = *DA_GET(services->services, tapper_entry_ptr, tapper_index);
        if (tapper->tag != TAPPER) {
            break; // we've found all the tappers, so we're done
        }
        o2_message_ptr o2msg = osc_to_o2(osc_len, osc_msg,
                                         tapper->tapper_name);
        o2_message_send_sched(o2msg, FALSE);
    }

    return O2_SUCCESS;
  close_socket:
    o2_service_free((void *) service->service_name);
    return O2_FAIL;
}
