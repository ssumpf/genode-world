/*
 * \brief  Entry point for POSIX applications
 * \author Norman Feske
 * \date   2016-12-23
 */

/*
 * Copyright (C) 2016-2020 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <libc/component.h>

/* libc includes */
#include <libc/args.h>
#include <stdlib.h> /* 'exit'   */

#include <libmbim-glib.h>
extern "C" {
#include <mbimcli.h>
}
/* initial environment for the FreeBSD libc implementation */
extern char **environ;

/* provided by the application */
extern "C" int main(int argc, char **argv, char **envp);

extern "C" unsigned char __bss_start;
extern "C" unsigned char _prog_img_end;

extern "C" void wait_for_continue();

static void clear_bss()
{
	Genode::log("CLEAR: ", &__bss_start, " size: ", &_prog_img_end - &__bss_start - 4);
	Genode::memset(&__bss_start, 0, &_prog_img_end - &__bss_start - 4);
}
static void construct_component(Libc::Env &env)
{
	int argc    = 5;
	char **envp = nullptr;

	enum { SEQ_COUNT = 5 };

//	populate_args_and_env(env, argc, argv, envp);
	char const *arg[SEQ_COUNT][6] =  {
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--enter-pin=1889", "HOME=/" },
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--query-registration-state", "HOME=/" },
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--attach-packet-service", "HOME=/" },
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--connect=apn='internet.eplus.de',auth='PAP',username='eplus',password='eplus'", "HOME=/" },
		{ "mbimcli", "--verbose", "-d", "/dev/cdc-wdm0", "--disconnect", "HOME=/" }
	};

	int ret = 0;
	Genode::log("BSS: ", &__bss_start, " - ", &_prog_img_end);
	for (unsigned i = 0; i < SEQ_COUNT /*SEQ_COUNT*/; i++) {
		envp = (char **)&arg[i][5];
		environ = envp;

		Genode::warning("call main: ", i, " args: ", arg[i][4]);
		ret = main(argc, (char **)arg[i], envp);
		Genode::warning("returned ", ret, " i: ", i);
		clear_bss();
	}

	Genode::warning("EXIT: ", ret);
	exit(ret);
}

class Main
{
	enum { TRACE = TRUE };

	enum State {
		NONE, QUERY, ATTACH, CONNECT };

	private:

		State       _state { NONE };
		GMainLoop  *_loop { nullptr };
		MbimDevice *_device { nullptr };
		unsigned    _retry { 0 };
		guint32     _session_id { 0 };

		static void _device_close_ready (MbimDevice   *dev,
		                                 GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			Main *main = reinterpret_cast<Main *>(user_data);
			g_autoptr(GError) error = nullptr;
			if (!mbim_device_close_finish(main->_device, res, &error)) {
				Genode::error("couldn't close device: ", (char const*)error->message);
				g_error_free(error);
			}

			g_main_loop_quit(main->_loop);
		}

		void _shutdown(gboolean operation_status)
		{
			/* Set the in-session setup */
			g_object_set(_device,
			             MBIM_DEVICE_IN_SESSION, FALSE,
			             NULL);

			/* Close the device */
			mbim_device_close(_device,
			                  15,
			                  nullptr,
			                  (GAsyncReadyCallback) _device_close_ready,
			                  this);
		}

		void _send_request()
		{
			Genode::log(__func__, " state: ", (unsigned)_state);
			g_autoptr(MbimMessage) request = nullptr;
			g_autoptr(GError)      error   = nullptr;

			switch (_state) {

				case NONE:
					request = mbim_message_register_state_query_new (NULL);
					mbim_device_command(_device,
					                    request,
					                    10,
					                    nullptr,
					                    (GAsyncReadyCallback)_register_state,
					                    this);
					break;

				case QUERY:
					request = mbim_message_packet_service_set_new(MBIM_PACKET_SERVICE_ACTION_ATTACH
					                                              , &error);
					if (!request) {
						Genode::error("couldn't create request: ", (char const *)error->message);
						_shutdown(FALSE);
						return;
					}

					mbim_device_command(_device,
					                    request,
					                    120,
					                    nullptr,
					                    (GAsyncReadyCallback)_packet_service_ready,
					                    this);
					break;

				case ATTACH: {
					//XXX: make configurable
					guint32            session_id { 0 };
					g_autofree gchar  *apn { (char *)"internet.eplus.de" };
					MbimAuthProtocol   auth_protocol { MBIM_AUTH_PROTOCOL_PAP };
					g_autofree gchar  *username { (char *)"eplus" };
					g_autofree gchar  *password { (char *)"eplus" };
					MbimContextIpType  ip_type { MBIM_CONTEXT_IP_TYPE_DEFAULT };

					request = mbim_message_connect_set_new(session_id,
					                                       MBIM_ACTIVATION_COMMAND_ACTIVATE,
					                                       apn,
					                                       username,
					                                       password,
					                                       MBIM_COMPRESSION_NONE,
					                                       auth_protocol,
					                                       ip_type,
					                                       mbim_uuid_from_context_type (MBIM_CONTEXT_TYPE_INTERNET),
					                                       &error);

					if (!request) {
						Genode::error("couldn't create request: ", (char const *)error->message);
						_shutdown (FALSE);
						return;
					}

					mbim_device_command (_device,
					                     request,
					                     120,
					                     nullptr,
					                     (GAsyncReadyCallback)_connect_ready,
					                     this);
					break;
				}

				case CONNECT:
					request = (mbim_message_ip_configuration_query_new (
					             _session_id,
					             MBIM_IP_CONFIGURATION_AVAILABLE_FLAG_NONE, /* ipv4configurationavailable */
					             MBIM_IP_CONFIGURATION_AVAILABLE_FLAG_NONE, /* ipv6configurationavailable */
					             0, /* ipv4addresscount */
					             NULL, /* ipv4address */
					             0, /* ipv6addresscount */
					             NULL, /* ipv6address */
					             NULL, /* ipv4gateway */
					             NULL, /* ipv6gateway */
					             0, /* ipv4dnsservercount */
					             NULL, /* ipv4dnsserver */
					             0, /* ipv6dnsservercount */
					             NULL, /* ipv6dnsserver */
					             0, /* ipv4mtu */
					             0, /* ipv6mtu */
					             &error));
					if (!request) {
						Genode::error("couldn't create IP config request: ", (char const *)error->message);
						_shutdown (FALSE);
						return;
					}

					mbim_device_command (_device,
					                     request,
					                     60,
					                     nullptr,
					                     (GAsyncReadyCallback)_ip_configuration_query_ready,
					                     this);
			}
		}

		static void _register_state(MbimDevice   *dev,
		                            GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			Main *main = reinterpret_cast<Main *>(user_data);
			g_autoptr(GError)      error    = nullptr;
			g_autoptr(MbimMessage) response = mbim_device_command_finish (main->_device, res, &error);

			if (!response ||
			    !mbim_message_response_get_result (response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error)) {
				Genode::error("operation failed: ", (char const*)error->message);
				main->_shutdown(FALSE);
				return;
			}

			MbimNwError          nw_error;
			MbimRegisterState    register_state;
			MbimRegisterMode     register_mode;
			MbimDataClass        available_data_classes;
			MbimCellularClass    cellular_class;
			g_autofree gchar    *provider_id = NULL;
			g_autofree gchar    *provider_name = NULL;
			g_autofree gchar    *roaming_text = NULL;
			MbimRegistrationFlag registration_flag;
			if (!mbim_message_register_state_response_parse(response,
			                                                &nw_error,
			                                                &register_state,
			                                                &register_mode,
			                                                &available_data_classes,
			                                                &cellular_class,
			                                                &provider_id,
			                                                &provider_name,
			                                                &roaming_text,
			                                                &registration_flag,
			                                                &error)) {
				Genode::error("couldn't parse response message: ", (char const *)error->message);
				main->_shutdown (FALSE);
				return;
			}

			Genode::log("Register state: ", (unsigned)register_state);
			if (register_state == MBIM_REGISTER_STATE_HOME ||
			    register_state == MBIM_REGISTER_STATE_ROAMING ||
			    register_state == MBIM_REGISTER_STATE_PARTNER)
				main->_state = Main::QUERY;

			if (main->_retry++ >= 5) {
				Genode::error("Device not registered");
				main->_shutdown(FALSE);
			}

			main->_send_request();
		}

		static void _packet_service_ready(MbimDevice   *dev,
		                                  GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			Main *main = reinterpret_cast<Main *>(user_data);
			GError *error                   = nullptr;
			g_autoptr(MbimMessage) response = mbim_device_command_finish (main->_device, res, &error);

			if (!response ||
			    !mbim_message_response_get_result (response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error)) {
				Genode::error("operation failed: ", (char const*)error->message);
				main->_shutdown(FALSE);
				return;
			}

			guint32                 nw_error;
			MbimPacketServiceState  packet_service_state;
			MbimDataClass           highest_available_data_class;
			g_autofree gchar       *highest_available_data_class_str = NULL;
			guint64                 uplink_speed;
			guint64                 downlink_speed;
			if (!mbim_message_packet_service_response_parse(response,
			                                                &nw_error,
			                                                &packet_service_state,
			                                                &highest_available_data_class,
			                                                &uplink_speed,
			                                                &downlink_speed,
			                                                &error)) {
				Genode::error("couldn't parse response message: ", (char const *)error->message);
				main->_shutdown (FALSE);
				return;
			}

			main->_state = Main::ATTACH;

			highest_available_data_class_str = mbim_data_class_build_string_from_mask (highest_available_data_class);
			Genode::log("Successfully attached packet service");
			Genode::log("Packet service status:\n",
			            "\tAvailable data classes: '", (char const *)highest_available_data_class_str, "'\n",
			            "\t          Uplink speed: '", uplink_speed, "'\n",
			            "\t        Downlink speed: '", downlink_speed, "'");
			main->_send_request();
		}

		static void _connect_ready(MbimDevice   *dev,
		                           GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			Main *main = reinterpret_cast<Main *>(user_data);
			GError *error                   = nullptr;
			g_autoptr(MbimMessage) response = mbim_device_command_finish (main->_device, res, &error);

			if (!response ||
			    !mbim_message_response_get_result (response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error)) {
				Genode::error("operation failed: ", (char const*)error->message);
				main->_shutdown(FALSE);
				return;
			}

			guint32                 session_id;
			MbimActivationState     activation_state;
			MbimVoiceCallState      voice_call_state;
			MbimContextIpType       ip_type;
			const MbimUuid         *context_type;
			guint32                 nw_error;
			if (!mbim_message_connect_response_parse (
			        response,
			        &session_id,
			        &activation_state,
			        &voice_call_state,
			        &ip_type,
			        &context_type,
			        &nw_error,
			        &error)) {
				Genode::error("couldn't parse response message: ", (char const *)error->message);
				main->_shutdown(FALSE);
				return;
			}

			Genode::log("connected");
			main->_session_id = session_id;
			main->_state      = Main::CONNECT;
			main->_send_request();
		}

		static void _device_open_ready(MbimDevice   *dev,
		                               GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			GError *error = NULL;
			Main *main = reinterpret_cast<Main *>(user_data);
			if (!mbim_device_open_finish(dev, res, &error)) {
				Genode::error("couldn't open the MbimDevice: ",
				              (char const *)error->message);
				exit (EXIT_FAILURE);
			}

			main->_retry = 0;
			main->_send_request();
			Genode::log("Message returned");
		}

		static void _ip_configuration_query_ready(MbimDevice   *dev,
		                                          GAsyncResult *res,
		                                          gpointer user_data)
		{
			Genode::log(__func__);
		}

		static void _device_new_ready(GObject *unsused, GAsyncResult *res, gpointer user_data)
		{
			Genode::log("device: ", (void *)user_data);
			Main *main = reinterpret_cast<Main *>(user_data);
			GError *error = NULL;
			MbimDeviceOpenFlags open_flags = MBIM_DEVICE_OPEN_FLAGS_NONE;

			main->_device = mbim_device_new_finish (res, &error);
			if (!main->_device) {
				Genode::error("couldn't create MbimDevice: ",
				              (char const*)error->message);
				exit (EXIT_FAILURE);
			}

			mbim_device_open_full(main->_device,
			                      open_flags,
			                      30,
			                      nullptr,
			                      (GAsyncReadyCallback) _device_open_ready,
			                      main);
		}

		static void _log_handler(const gchar *log_domain,
		                         GLogLevelFlags log_level,
		                         const gchar *message,
		                         gpointer user_data)
		{
			Genode::String<32> level;
			switch (log_level) {
				case G_LOG_LEVEL_WARNING:
					level = "[Warning]";
					break;
				case G_LOG_LEVEL_CRITICAL:
				case G_LOG_LEVEL_ERROR:
					level = "[Error]";
					break;
				case G_LOG_LEVEL_DEBUG:
					level = "[Debug]";
					break;
				case G_LOG_LEVEL_MESSAGE:
				case G_LOG_LEVEL_INFO:
					level = "[Info]";
				break;
				case G_LOG_FLAG_FATAL:
				case G_LOG_LEVEL_MASK:
				case G_LOG_FLAG_RECURSION:
				default:
					g_assert_not_reached ();
			}

				Genode::log(level, " ", message);
		}

		void _init()
		{
			if (TRACE) {
				g_log_set_handler (NULL, G_LOG_LEVEL_MASK, _log_handler, NULL);
				g_log_set_handler ("Mbim", G_LOG_LEVEL_MASK, _log_handler, NULL);
			}
			mbim_utils_set_traces_enabled(TRACE);

			g_autoptr(GFile) file = NULL;
			file = g_file_new_for_commandline_arg ("/dev/cdc-wdm0");
			_loop = g_main_loop_new (NULL, FALSE);
			Genode::log("new device: ", this, " tracing enabled: ", mbim_utils_get_traces_enabled());
			mbim_device_new (file, nullptr, (GAsyncReadyCallback)_device_new_ready, this);
		}

		void _connect()
		{
			g_main_loop_run(_loop);
			Genode::log("RETURNED");
			g_object_unref(_device);
			g_main_loop_unref(_loop);
		}
	public:

		Main(Libc::Env &env)
		{
			_init();
			_connect();
			exit(0);
		}
};


void Libc::Component::construct(Libc::Env &env)
{
	Libc::with_libc([&] () {
		static Main main { env };
	});
}
