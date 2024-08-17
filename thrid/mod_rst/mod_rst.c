/*
auth:上海宁卫信息技术有限公司
License: GPL
Description: this is a module of FreeSWITCH，and it send any udp stream to other udp server
*/

#include "mod_rst.h"
#include "nway_log.h"
SWITCH_MODULE_LOAD_FUNCTION(mod_rst_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_rst_shutdown);
SWITCH_MODULE_DEFINITION(mod_rst, mod_rst_load, mod_rst_shutdown, NULL);

static struct
{
	switch_memory_pool_t *pool;
	switch_mutex_t *mutex;
	char *udp_server_ip;
	unsigned short udp_server_port;
	unsigned int fs_ver;
	switch_hash_t *port_map;
	int write_hex;
} globals;

typedef struct rst_helper
{

	int native;
	uint32_t packet_len;
	int min_sec;
	int final_timeout_ms;
	int initial_timeout_ms;
	int silence_threshold;
	int silence_timeout_ms;
	switch_time_t silence_time;
	int rready;
	int wready;
	switch_time_t last_read_time;
	switch_time_t last_write_time;
	switch_bool_t hangup_on_error;
	switch_codec_implementation_t read_impl;
	switch_codec_implementation_t write_impl;
	switch_bool_t speech_detected;
	switch_buffer_t *thread_buffer;
	switch_thread_t *thread;
	switch_mutex_t *buffer_mutex;
	int thread_ready;
	const char *completion_cause;
	char *udp_server_ip; // 可以每次来指定ip:port

	unsigned short udp_server_port; // port

	char *local_ip;
	switch_port_t local_port;
	//remote_ip and remote_port can tell me from udp server,so can more machine for this
	char *remote_ip;//
	switch_port_t remote_port;

	char *uuid;
	char *caller;
	char *callee;
	switch_core_session_t *session;
	// switch_audio_resampler_t *resampler;
	switch_socket_t *socket = NULL;
	switch_sockaddr_t *remote_addr = NULL;
	switch_sockaddr_t *local_addr = NULL;
	switch_sockaddr_t *from_addr = NULL;
	switch_memory_pool_t *pool = NULL;
	////debug file
	FILE *caller_file_hex;
	FILE *callee_file_hex;
} rst_helper_t;

typedef struct rst_thread_helper
{
	/* data */
	switch_core_session_t *session;
	char *ip;
	short port;
	switch_memory_pool_t *pool;
} rst_thread_helper_t;
typedef struct nway_caller
{
	char *username;
	int caller_type;

} nway_caller_t;
static void nway_get_caller(nway_caller_t *caller, const char *channel_name)
{
	char *p;
	if (!strncmp(channel_name, "sofia/external", 14))
	{
		p = strchr(channel_name + 14, '/');
		if (p)
		{
			caller->username = p + 1;
			caller->caller_type = 1;
		}
	}
	else if (!strncmp(channel_name, "sofia/internal/", 15))
	{
		caller->username = (char *)channel_name + 15;
		caller->caller_type = 0;
	}
	if (caller->username && (p = strchr(caller->username, '@')))
	{
		*p++ = '\0';
	}
}

SWITCH_DECLARE(char *)
switch_uuid_str(char *buf, switch_size_t len)
{
	switch_uuid_t uuid;

	if (len < (SWITCH_UUID_FORMATTED_LENGTH + 1))
	{
		switch_snprintf(buf, len, "INVALID");
	}
	else
	{
		switch_uuid_get(&uuid);
		switch_uuid_format(buf, &uuid);
	}

	return buf;
}

static switch_bool_t nway_is_silence_frame(switch_frame_t *frame, int silence_threshold, switch_codec_implementation_t *codec_impl)
{
	int16_t *fdata = (int16_t *)frame->data;
	uint32_t samples = frame->datalen / sizeof(*fdata);
	switch_bool_t is_silence = SWITCH_TRUE;
	uint32_t channel_num = 0;

	int divisor = 0;
	if (!(divisor = codec_impl->samples_per_second / 8000))
	{
		divisor = 1;
	}

	/* is silence only if every channel is silent */
	for (channel_num = 0; channel_num < codec_impl->number_of_channels && is_silence; channel_num++)
	{
		uint32_t count = 0, j = channel_num;
		double energy = 0;
		for (count = 0; count < samples; count++)
		{
			energy += abs(fdata[j]);
			j += codec_impl->number_of_channels;
		}
		is_silence &= (uint32_t)((energy / (samples / divisor)) < silence_threshold);
	}

	return is_silence;
}

static switch_status_t nway_send_to(struct rst_helper *rh, char *data, int len)
{
	switch_size_t mylen = len;
	if (rh->socket)
	{
		// log_ntc("%s   len:%d",data,len);
		switch_status_t status = switch_socket_sendto(rh->socket, rh->remote_addr, 0, (void *)data, &mylen);

		return status;
	}
	return SWITCH_STATUS_GENERR;
}
static void InsertHexFile(char *data, int len, FILE *f)
{
	char *tmp = malloc(4000);
	char *tmp2 = malloc(10);
	int i = 0;
	memset(tmp, 0, 4000);
	memset(tmp2, 0, 10);
	if (!tmp)
	{
		log_ntc("can not malloc memory");
		return;
	}
	for (; i < len; i++)
	{
		sprintf(tmp2, " 0x%x", data[i]);
		strcat(tmp, tmp2);
	}

	fwrite(tmp, strlen(tmp), 1, f);

	switch_safe_free(tmp2);
	switch_safe_free(tmp);
}

static switch_bool_t nway_rst_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	rst_helper_t *rh = (rst_helper_t *)user_data;
	switch_event_t *event;
	switch_frame_t *nframe;
	switch_size_t len = 0;

	switch_codec_t *raw_codec = NULL;
	switch_codec_implementation_t read_impl = {0};
	int mask = switch_core_media_bug_test_flag(bug, SMBF_MASK);
	unsigned char null_data[SWITCH_RECOMMENDED_BUFFER_SIZE] = {0};

	int16_t *read_data;
	int read_samples;

	switch_core_session_get_read_impl(session, &read_impl);

	int channels = read_impl.number_of_channels;

	switch_assert(session);

	// if (switch_channel_down(channel)) {
	//	return SWITCH_FALSE;
	// }

	switch (type)
	{
	case SWITCH_ABC_TYPE_INIT:
	{
		// get caller,callee,uuid,caller_port,callee_port
		
		char cmd[1024] = {0};
		
		if (globals.write_hex)
		{
			// create two file
			sprintf(cmd, "/opt/nway/records/%s_%s.hex\0", rh->uuid, rh->caller);
			rh->caller_file_hex = fopen(cmd, "w+");
			if (rh->caller_file_hex == NULL)
			{
				log_err("open caller file hex failed");
			}
			sprintf(cmd, "/opt/nway/records/%s_%s.hex\0", rh->uuid, rh->callee);
			rh->callee_file_hex = fopen(cmd, "w+");
			if (rh->callee_file_hex == NULL)
			{
				log_err("open caller file hex failed");
			}
		}
		sprintf(cmd, "%s:%s:%s:%s\0", INV, rh->uuid, rh->caller, rh->callee);
		log_ntc(cmd);

		nway_send_to(rh, cmd, strlen(cmd));
		//if (nway_cmd_consult(rh)!=SWITCH_STATUS_SUCCESS){
			//remove this media bug
		//	switch_core_media_bug_remove_callback(session, nway_rst_callback);
		//}
		//reconnect a new socket from new remote ip,remote port
	}
	break;
	case SWITCH_ABC_TYPE_TAP_NATIVE_READ:
	{

		switch_frame_t *rframe = NULL;

		rframe = switch_core_media_bug_get_native_read_frame(bug);
		switch_size_t len;

		char payload[20] = {0};
		char cmd[2048] = {0};

		raw_codec = switch_core_session_get_read_codec(session);
    		if (raw_codec) {
        		uint32_t read_sample_rate = raw_codec->implementation->samples_per_second;
        		uint32_t read_channels = raw_codec->implementation->number_of_channels;
        		const char *read_codec_name = raw_codec->implementation->iananame;

        		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "jzq media-bug pt:%d Read codec: %s, Sample rate: %u Hz, Channels: %u\n", raw_codec->agreed_pt, read_codec_name, read_sample_rate, read_channels);
    		}
		if (raw_codec->agreed_pt == 8)
		{
			// alaw
			strcpy(payload, "08");
			len = 160;
		}
		else if (raw_codec->agreed_pt == 0)
		{
			// ulaw
			strcpy(payload, "00");
			len = 160;
		}
		else if (raw_codec->agreed_pt == 10)
		{
			// pcm
			strcpy(payload, "10");
			len = 160 * 2;
		}
		sprintf(cmd, "DATA:%s:R:%s:%d:", rh->uuid, payload, len);
		len = strlen(cmd);
		char *f = cmd;
		if (globals.write_hex)
			InsertHexFile(rframe->data, rframe->samples, rh->caller_file_hex);
		// memcpy(cmd + len, rframe->data, (switch_size_t)rframe->datalen);
		memcpy(cmd + len, rframe->data, rframe->samples);
		len = len + rframe->samples;
		nway_send_to(rh, cmd, len);
	}
	break;
	case SWITCH_ABC_TYPE_TAP_NATIVE_WRITE:
	{
		switch_frame_t *wframe = switch_core_media_bug_get_native_write_frame(bug);
		switch_size_t len;

		char payload[2] = {0};
		char cmd[1024] = {0};

		raw_codec = switch_core_session_get_read_codec(session);
		if (raw_codec->agreed_pt == 8)
		{
			// alaw
			strcpy(payload, "08");
			len = 160;
		}
		else if (raw_codec->agreed_pt == 0)
		{
			// ulaw
			strcpy(payload, "00");
			len = 160;
		}
		else if (raw_codec->agreed_pt == 10)
		{
			// pcm
			strcpy(payload, "10");
			len = 160 * 2;
		}
		sprintf(cmd, "DATA:%s:W:%s:%d:", rh->uuid, payload, len);
		len = strlen(cmd);
		char *f = cmd;
		memcpy(cmd + len, wframe->data, wframe->samples);
		if (globals.write_hex)
			InsertHexFile(wframe->data, wframe->datalen, rh->callee_file_hex);
		len = len + wframe->samples;
		nway_send_to(rh, cmd, len);
	}

	break;
	case SWITCH_ABC_TYPE_CLOSE:
	{

		char cmd[1024] = {0};
		sprintf(cmd, "BYE :%s\0", rh->uuid);
		log_ntc(cmd);
		nway_send_to(rh, cmd, strlen(cmd));

		if (rh->socket)
		{
			switch_socket_shutdown(rh->socket, SWITCH_SHUTDOWN_READWRITE);
			switch_socket_close(rh->socket);
		}
		if (rh->pool)
		{
			switch_core_destroy_memory_pool(&rh->pool);
		}
		if (globals.write_hex)
		{
			if (rh->callee_file_hex)
				fclose(rh->callee_file_hex);
			if (rh->caller_file_hex)
				fclose(rh->caller_file_hex);
		}
	}
	break;
	case SWITCH_ABC_TYPE_READ_PING:

		break;

	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_status_t switch_find_available_port(switch_port_t *port, const char *ip, int type)
{
	switch_status_t ret = SWITCH_STATUS_SUCCESS;
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *addr = NULL;
	switch_socket_t *sock = NULL;
	switch_bool_t found = SWITCH_FALSE;

	if ((ret = switch_core_new_memory_pool(&pool)) != SWITCH_STATUS_SUCCESS)
	{
		goto done;
	}

	while (!found)
	{
		if ((ret = switch_sockaddr_info_get(&addr, ip, SWITCH_UNSPEC, *port, 0, pool)) != SWITCH_STATUS_SUCCESS)
		{
			goto done;
		}

		if ((ret = switch_socket_create(&sock, switch_sockaddr_get_family(addr), type, 0, pool)) != SWITCH_STATUS_SUCCESS)
		{
			goto done;
		}

		// if (!(found = (switch_socket_bind(sock, addr) == SWITCH_STATUS_SUCCESS))) {
		if (switch_socket_bind(sock, addr) == SWITCH_STATUS_SUCCESS)
		{
			found = SWITCH_FALSE;
			*port = *port + 1;
		}
		else
		{
			found = SWITCH_TRUE;
		}

		switch_socket_close(sock);
	}

done:
	if (pool)
		switch_core_destroy_memory_pool(&pool);

	return ret;
}

SWITCH_DECLARE(switch_status_t)
nway_rst_session(switch_core_session_t *session, const char *ip, short port)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "concurrent is:%d,18621575908 \n", switch_core_session_count());
	switch_channel_t *channel = switch_core_session_get_channel(session);
	// const char *p;
	// const char *vval;
	switch_media_bug_t *bug;
	switch_status_t status;
	time_t to = 0;
	const char *channel_name = switch_channel_get_variable(channel, "channel_name");
	nway_caller_t caller = {0};
	switch_media_bug_flag_t flags = SMBF_TAP_NATIVE_READ | SMBF_TAP_NATIVE_WRITE | SMBF_READ_PING | SMBF_READ_STREAM | SMBF_WRITE_STREAM;
	// SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING;// SMBF_TAP_NATIVE_READ | SMBF_TAP_NATIVE_WRITE  ;
	//  SMBF_READ_STREAM | SMBF_WRITE_STREAM |SMBF_STEREO| SMBF_READ_PING |SMBF_STEREO;SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING ;//
	uint8_t channels;
	switch_codec_implementation_t read_impl = {0};
	struct rst_helper *rh = NULL;
	int file_flags = SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT;
	switch_bool_t hangup_on_error = SWITCH_FALSE;
	switch_codec_t raw_codec = {0};
	char local_ip[64];
	switch_port_t local_port = 6050;

	if (!switch_channel_media_up(channel) || !switch_core_session_get_read_codec(session))
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can not rst session.  Media not enabled on channel\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_core_session_get_read_impl(session, &read_impl);
	channels = read_impl.number_of_channels;
	rh = switch_core_session_alloc(session, sizeof(*rh));
	// if (read_impl.actual_samples_per_second != 8000) {

	// 	if (switch_resample_create(&rh->resampler,
	// 		read_impl.actual_samples_per_second,
	// 		8000,
	// 		read_impl.samples_per_packet, SWITCH_RESAMPLE_QUALITY, 1) != SWITCH_STATUS_SUCCESS) {

	// 			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Unable to create resampler!\n");

	// 			goto end;

	// 	}
	// }

	// 优先为每次调用时的，否则使用全局的
	if (strlen(ip) < 4 || port == 0)
	{
		if (switch_sockaddr_info_get(&rh->remote_addr, globals.udp_server_ip, SWITCH_UNSPEC,
									 globals.udp_server_port, 0, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS)
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Socket Error 3\n");
			log_ntc("remote ip:%s    port:%d address error\n", globals.udp_server_ip, globals.udp_server_port);
			goto end;
		}
	}
	else
	{
		if (switch_sockaddr_info_get(&rh->remote_addr, ip, SWITCH_UNSPEC,
									 port, 0, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS)
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Socket Error 3\n");
			log_ntc("remote ip:%s    port:%d address error\n", ip, port);
			goto end;
		}
	}

	// log_ntc("create socket");
	///////////////////////
	// switch_socket_sendto(globals.udp_socket, globals.dst_sockaddrs[i].sockaddr, 0, buf, &len);

	if (switch_socket_create(&rh->socket, AF_INET, SOCK_DGRAM, 0, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Socket Error 1\n");
		goto end;
	}

	// 本地ip 和port

	switch_find_local_ip(local_ip, sizeof(local_ip), NULL, AF_INET);

	/*if (switch_find_available_port(&local_port, local_ip, SOCK_DGRAM) != SWITCH_STATUS_SUCCESS) {
		log_err( "RST failed: could not get available local port\n");

		goto end;
	}*/
	local_port = 0;
	if (switch_sockaddr_info_get(&rh->local_addr, local_ip, SWITCH_UNSPEC,
								 local_port, 0, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Socket Error local address\n");
		goto end;
	}
	log_ntc("local ip:%s    port:%d\n", local_ip, local_port);
	if (switch_socket_bind(rh->socket, rh->local_addr) != SWITCH_STATUS_SUCCESS)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to bind IPv4 Socket\n");
	}
	else
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "IPv4 source set to: %s\n", rh->local_addr);
	}
	switch_socket_opt_set(rh->socket, SWITCH_SO_NONBLOCK, TRUE);
	nway_get_caller(&caller, channel_name);
	if (!zstr(caller.username))
	{
		rh->caller = switch_core_strdup(switch_core_session_get_pool(session), caller.username);
		rh->uuid = switch_core_strdup(switch_core_session_get_pool(session), switch_core_session_get_uuid(session));
		rh->callee = switch_core_strdup(switch_core_session_get_pool(session), switch_channel_get_variable(channel, "destination_number"));
	}
	else
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "No caller name\n");
		goto end;
	}
	rh->session = session;
	if ((status = switch_core_media_bug_add(session, "rst", NULL,
											nway_rst_callback, rh, to, flags, &bug)) != SWITCH_STATUS_SUCCESS)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error adding media bug  \n");

		goto end;
	}
	return SWITCH_STATUS_SUCCESS;
end:
	// if (rh->resampler) {
	// 	switch_resample_destroy(&rh->resampler);
	// }
	log_err("create socket error:%s:%d\n", globals.udp_server_ip, globals.udp_server_port);
	if (rh->socket)
		switch_socket_close(rh->socket);
	if (rh->pool)
	{
		switch_core_destroy_memory_pool(&rh->pool);
	}
	switch_core_session_reset(session, SWITCH_FALSE, SWITCH_TRUE);
	return SWITCH_STATUS_FALSE;
}

static switch_status_t load_config(void)
{
	char *cf = "rst.conf";
	switch_xml_t cfg, xml = NULL, param, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL)))
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	globals.write_hex = 0;
	if ((settings = switch_xml_child(cfg, "settings")))
	{
		for (param = switch_xml_child(settings, "param"); param; param = param->next)
		{
			char *var = (char *)switch_xml_attr_soft(param, "name");
			char *val = (char *)switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "udp-server-ip"))
			{
				if (!zstr(val))
				{
					globals.udp_server_ip = switch_core_strdup(globals.pool, val);
				}
			}
			else if (!strcasecmp(var, "udp-server-port"))
			{
				if (!zstr(val))
				{
					globals.udp_server_port = atoi(val);
				}
			}
			else if (!strcasecmp(var, "write-hex"))
			{
				if (!zstr(val) && !strcasecmp(val, "true"))
				{
					globals.write_hex = 1;
				}
				else
				{
					globals.write_hex = 0;
				}
			}
		}
	}
done:

	if (xml)
	{
		switch_xml_free(xml);
	}

	return status;
}

SWITCH_DECLARE(switch_status_t)
switch_ivr_stop_rts_session(switch_core_session_t *session)
{
	switch_media_bug_t *bug;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	return switch_core_media_bug_remove_callback(session, nway_rst_callback);
}
#define STOP_RST_SESSION_SYNC "stop_rst <uuid>\n"
SWITCH_STANDARD_API(stop_rst_session_function)
{
	switch_core_session_t *rsession = NULL;
	if (zstr(cmd))
	{
		goto usage;
	}
	if (!(rsession = switch_core_session_locate(cmd)))
	{
		stream->write_function(stream, "-ERR Cannot locate session!\n");
		goto fail;
	}
	switch_ivr_stop_rts_session(rsession);
	return SWITCH_STATUS_SUCCESS;
usage:

	stream->write_function(stream, "-USAGE: %s\n", STOP_RST_SESSION_SYNC);
	return SWITCH_STATUS_FALSE;
fail:
	return SWITCH_STATUS_FALSE;
}

SWITCH_STANDARD_APP(rst_session_function)
{

	char *ip = NULL;
	short port = 0;

	char *mycmd = NULL, *argv[10] = {0};
	char *uuid = NULL;
	int argc = 0;

	switch_memory_pool_t *pool = NULL;
	pool = switch_core_session_get_pool(session);
	if (zstr(data) && !(mycmd = switch_core_strdup(pool, data)))
	{
		log_err("no enough memory\n");
		goto fail;
	}

	if ((argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) < 2)
	{
		log_err("no enough parameters,but continue!\n");
		nway_rst_session(session, "", 0);
	}
	else
	{
		ip = switch_core_strdup(pool, argv[0]);
		port = atoi(argv[1]);
		nway_rst_session(session, ip, port);
	}
	return;
fail:
	log_err("err: exit session function\n");
	return;
}

void *SWITCH_THREAD_FUNC nway_rts_thread_function(switch_thread_t *thread, void *obj)
{
	rst_thread_helper_t *rth = (rst_thread_helper_t *)obj;
	nway_rst_session(rth->session, rth->ip, rth->port);
	// 在线程运行结束后，需要把pool释放
	if (rth && rth->pool)
	{
		switch_core_destroy_memory_pool(&rth->pool);
	}
}

switch_thread_t *start_nway_rts_thread(rst_thread_helper_t *rth)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;

	switch_threadattr_create(&thd_attr, rth->pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_threadattr_priority_set(thd_attr, SWITCH_PRI_REALTIME);
	switch_thread_create(&thread, thd_attr, nway_rts_thread_function, rth, rth->pool);
	return thread;
}

#define SESSIOS_RST_SYNTAX "<uuid> <ip> <port>"
SWITCH_STANDARD_API(session_rst_function)
{
	switch_core_session_t *rsession = NULL;
	rst_thread_helper_t *rth = NULL;
	switch_memory_pool_t *pool = NULL;
	char *mycmd = NULL, *argv[10] = {0};
	char *uuid = NULL;
	int argc = 0;
	uint32_t limit = 0;

	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS)
	{
		log_err("create pool fail\n");
		goto fail;
	}

	if (!(rth = (rst_thread_helper_t *)switch_core_alloc(pool, sizeof(rst_thread_helper_t))))
	{
		log_err("no enough memory\n");
		goto fail;
	}

	if (zstr(cmd))
	{
		goto usage;
	}

	if (!(mycmd = switch_core_strdup(pool, cmd)))
	{
		log_err("no enough memory\n");
		goto fail;
	}

	if ((argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) < 3)
	{
		log_err("no enough parameters\n");
		goto fail;
	}
	uuid = switch_core_strdup(pool, argv[0]);
	if (!(rsession = switch_core_session_locate(uuid)))
	{
		stream->write_function(stream, "-ERR Cannot locate session!\n");
		goto fail;
	}
	rth->session = rsession;
	rth->pool = pool;
	rth->ip = switch_core_strdup(pool, argv[1]);
	rth->port = atoi(argv[2]);
	goto done;

usage:
	stream->write_function(stream, "-USAGE: %s\n", SESSIOS_RST_SYNTAX);
	return SWITCH_STATUS_FALSE;
done:
	// start a rst thread
	start_nway_rts_thread(rth);
	if (rsession)
	{
		switch_core_session_rwunlock(rsession);
	}
	return SWITCH_STATUS_SUCCESS;
fail:

	if (pool)
	{
		switch_core_destroy_memory_pool(&pool);
	}

	stream->write_function(stream, "create session fail\n");
	return SWITCH_STATUS_FALSE;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_rst_load)
{
	switch_application_interface_t *app_interface;

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;

	switch_api_interface_t *api_interface;
	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, globals.pool);
	unsigned int major = atoi(switch_version_major());
	unsigned int minor = atoi(switch_version_minor());
	unsigned int micro = atoi(switch_version_micro());

	globals.fs_ver = major << 16;
	globals.fs_ver |= minor << 8;
	globals.fs_ver |= micro << 4;
	load_config();
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_APP(app_interface, "rst", "realtime stream transfer", "rst ip port", rst_session_function, "", SAF_MEDIA_TAP);
	SWITCH_ADD_API(api_interface, "rst_stop", "realtime stream transfer", stop_rst_session_function, STOP_RST_SESSION_SYNC);
	SWITCH_ADD_API(api_interface, "uuid_rst", "realtime stream transfer api", session_rst_function, SESSIOS_RST_SYNTAX);
	switch_core_hash_init(&globals.port_map);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " rst loaded\n");
	return SWITCH_STATUS_SUCCESS;
END:
	return SWITCH_STATUS_FALSE;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_rst_shutdown)
{
	switch_core_hash_destroy(&globals.port_map);
	switch_mutex_destroy(globals.mutex);
	return SWITCH_STATUS_SUCCESS;
}
