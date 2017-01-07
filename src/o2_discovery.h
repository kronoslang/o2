//
//  o2_discovery.h
//  O2
//
//  Created by 弛张 on 1/26/16.
//  Copyright © 2016 弛张. All rights reserved.
//

#ifndef o2_discovery_h
#define o2_discovery_h

#define PORT_MAX  16

extern SOCKET o2_discovery_socket;
extern int o2_port_map[16];

/**
 *  Initialize for discovery 
 *
 *  @return O2_SUCCESS (0) if succeed, O2_FAIL (-1) if not.
 */
int o2_discovery_initialize();

int o2_discovery_finish();

int o2_discovery_msg_initialize();

/**
 *  Discover function will send the discover messages and deal with all the discover
 *  messages sent to the discover socket. Record all the information in the
 *  remote_process arrays and periodly check for update. If there exists a new
 *  remote_process, the o2_discover() will automatically set up a new tcp
 *  connection with the remote_process.
 *
 *  @return 0 if succeed, 1 if there is some error.
 */
void o2_discovery_send_handler(o2_msg_data_ptr msg, const char *types,
                               o2_arg_ptr *argv, int argc, void *user_data);

int o2_send_initialize(fds_info_ptr process);

int o2_send_services(fds_info_ptr process);

void o2_discovery_handler(o2_msg_data_ptr msg, const char *types,
                          o2_arg_ptr *argv, int argc, void *user_data);

void o2_discovery_init_handler(o2_msg_data_ptr msg, const char *types,
                               o2_arg_ptr *argv, int argc, void *user_data);


void o2_services_handler(o2_msg_data_ptr msg, const char *types,
                         o2_arg_ptr *argv, int argc, void *user_data);

#endif /* O2_discovery_h */
