/*
 * \brief  MBIM connection bindings
 * \author Sebastian Sumpf
 * \date   2020-10-27
 */

/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Genode includes */
#include <libc/component.h>
#include <net/ipv4.h>

/* libc includes */
#include <libc/args.h>
#include <stdlib.h> /* 'exit'   */

#include <libmbim-glib.h>
extern "C" {
#include <mbimcli.h>
}

class Mbim
{
	enum { TRACE = TRUE };

	enum State { NONE, PIN, QUERY, ATTACH, CONNECT };

	struct Connection {
		Net::Ipv4_address ip;
		Net::Ipv4_address mask;
		Net::Ipv4_address gateway;
		Net::Ipv4_address dns[2];
	};

	private:

		State       _state { NONE };
		GMainLoop  *_loop { nullptr };
		MbimDevice *_device { nullptr };
		unsigned    _retry { 0 };
		guint32     _session_id { 0 };
		Connection  _connection { };

		static Mbim *_mbim(gpointer user_data)
		{
			return reinterpret_cast<Mbim *>(user_data);
		}

		MbimMessage *_command_response(GAsyncResult *res, bool shutdown = true)
		{
			GError *error         = nullptr;
			MbimMessage *response = mbim_device_command_finish (_device, res, &error);

			if (!response ||
			    !mbim_message_response_get_result (response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error)) {
				Genode::error("operation failed: ", (char const*)error->message);
				if (shutdown)
					_shutdown(FALSE);
				return nullptr;
			}

			return response;
		}

		gchar const *_pin()
		{
			//XXX: make configurable
			return "1889";
		}

		static void _device_close_ready (MbimDevice   *dev,
		                                 GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			Mbim *mbim = reinterpret_cast<Mbim *>(user_data);
			g_autoptr(GError) error = nullptr;
			if (!mbim_device_close_finish(mbim->_device, res, &error)) {
				Genode::error("couldn't close device: ", (char const*)error->message);
				g_error_free(error);
			}

			g_main_loop_quit(mbim->_loop);
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
					request = (mbim_message_pin_set_new(MBIM_PIN_TYPE_PIN1,
					                                    MBIM_PIN_OPERATION_ENTER,
					                                    _pin(),
					                                    nullptr,
					                                    &error));
					if (!request) {
						Genode::error("couldn't create request: ", (char const*)error->message);
						_shutdown (FALSE);
						return;
					}

					mbim_device_command (_device,
					                     request,
					                     10,
					                     nullptr,
					                     (GAsyncReadyCallback)_pin_ready,
					                     this);
					break;

				case PIN:
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

		static void _pin_ready(MbimDevice   *dev,
		                       GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			Mbim *mbim = _mbim(user_data);
			g_autoptr(GError)      error    = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res, false);

			if (!response) {
				Genode::log("PIN might be entered already");
				mbim->_state = Mbim::PIN;
				mbim->_send_request();
				return;
			}

			MbimPinType             pin_type;
			MbimPinState            pin_state;
			guint32                 remaining_attempts;
			if (!mbim_message_pin_response_parse (
			        response,
			        &pin_type,
			        &pin_state,
			        &remaining_attempts,
			        &error)) {
				Genode::error("couldn't parse response message: ", (char const *)error->message);
				mbim->_shutdown (FALSE);
				return;
			}

			Genode::log("PIN: state: ", mbim_pin_state_get_string(pin_state),
			            " remaining attempts: ", remaining_attempts);
			mbim->_state = Mbim::PIN;
			mbim->_send_request();
		}

		static void _register_state(MbimDevice   *dev,
		                            GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			Mbim *mbim = _mbim(user_data);
			g_autoptr(GError)      error    = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res);

			if (!response) return;

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
				mbim->_shutdown (FALSE);
				return;
			}

			Genode::log("Register state: ", (unsigned)register_state);
			if (register_state == MBIM_REGISTER_STATE_HOME ||
			    register_state == MBIM_REGISTER_STATE_ROAMING ||
			    register_state == MBIM_REGISTER_STATE_PARTNER)
				mbim->_state = Mbim::QUERY;

			if (mbim->_retry++ >= 100) {
				Genode::error("Device not registered after ", mbim->_retry, " tries");
				mbim->_shutdown(FALSE);
			}

			mbim->_send_request();
		}

		static void _packet_service_ready(MbimDevice   *dev,
		                                  GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			Mbim *mbim = _mbim(user_data);
			GError *error                   = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res);

			if (!response) return;

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
				mbim->_shutdown (FALSE);
				return;
			}

			mbim->_state = Mbim::ATTACH;

			highest_available_data_class_str = mbim_data_class_build_string_from_mask (highest_available_data_class);
			Genode::log("Successfully attached packet service");
			Genode::log("Packet service status:\n",
			            "\tAvailable data classes: '", (char const *)highest_available_data_class_str, "'\n",
			            "\t          Uplink speed: '", uplink_speed, "'\n",
			            "\t        Downlink speed: '", downlink_speed, "'");
			mbim->_send_request();
		}

		static void _connect_ready(MbimDevice   *dev,
		                           GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			Mbim *mbim = _mbim(user_data);
			GError *error                   = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res);

			if (!response) return;

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
				mbim->_shutdown(FALSE);
				return;
			}

			Genode::log("connected");
			mbim->_session_id = session_id;
			mbim->_state      = Mbim::CONNECT;
			mbim->_send_request();
		}

		static void _ip_configuration_query_ready(MbimDevice   *dev,
		                                          GAsyncResult *res,
		                                          gpointer user_data)
		{
			Genode::log(__func__);
			Mbim *mbim = _mbim(user_data);
			GError *error                   = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res);

			if (!response) return;

			MbimIPConfigurationAvailableFlag  ipv4configurationavailable;
			g_autofree gchar                 *ipv4configurationavailable_str = NULL;
			MbimIPConfigurationAvailableFlag  ipv6configurationavailable;
			g_autofree gchar                 *ipv6configurationavailable_str = NULL;
			guint32                           ipv4addresscount;
			g_autoptr(MbimIPv4ElementArray)   ipv4address = NULL;
			guint32                           ipv6addresscount;
			g_autoptr(MbimIPv6ElementArray)   ipv6address = NULL;
			const MbimIPv4                   *ipv4gateway;
			const MbimIPv6                   *ipv6gateway;
			guint32                           ipv4dnsservercount;
			g_autofree MbimIPv4              *ipv4dnsserver = NULL;
			guint32                           ipv6dnsservercount;
			g_autofree MbimIPv6              *ipv6dnsserver = NULL;
			guint32                           ipv4mtu;
			guint32                           ipv6mtu;

			if (!mbim_message_ip_configuration_response_parse (
			        response,
			        NULL, /* sessionid */
			        &ipv4configurationavailable,
			        &ipv6configurationavailable,
			        &ipv4addresscount,
			        &ipv4address,
			        &ipv6addresscount,
			        &ipv6address,
			        &ipv4gateway,
			        &ipv6gateway,
			        &ipv4dnsservercount,
			        &ipv4dnsserver,
			        &ipv6dnsservercount,
			        &ipv6dnsserver,
			        &ipv4mtu,
			        &ipv6mtu,
			        &error))
			return;

			if (!ipv4configurationavailable) {
				Genode::error("No ipv4 configuration available");
				return;
			}

			Net::Ipv4_address address { ipv4address[0]->ipv4_address.addr };

			Genode::uint32_t netmask_lower = 32 - ipv4address[0]->on_link_prefix_length;
			Genode::uint32_t netmask = ~0u;
			for (Genode::uint32_t i = 0; i < netmask_lower; i++) {
				netmask ^= (1 << i);
			}

			Net::Ipv4_address mask { Net::Ipv4_address::from_uint32_little_endian(netmask) };
			Net::Ipv4_address gateway { (void *)ipv4gateway->addr };
			Genode::log("ip     : ", address);
			Genode::log("mask   : ", mask);
			Genode::log("gateway: ", gateway);

			Net::Ipv4_address dns[2];
			for (Genode::uint32_t i = 0; i < ipv4dnsservercount && i < 2; i++) {
				dns[i] = Net::Ipv4_address { (void *)ipv4dnsserver[i].addr };
				Genode::log("dns", i, "   : ", dns[i]);
			}

			mbim->_connection.ip      = address;
			mbim->_connection.mask    = mask;
			mbim->_connection.gateway = gateway;
			mbim->_connection.dns[0]  = dns[0];
			mbim->_connection.dns[1]  = dns[1];
			mbim->_report();
		}

		static void _device_open_ready(MbimDevice   *dev,
		                               GAsyncResult *res, gpointer user_data)
		{
			Genode::log(__func__);
			GError *error = NULL;
			Mbim *mbim = reinterpret_cast<Mbim *>(user_data);
			if (!mbim_device_open_finish(dev, res, &error)) {
				Genode::error("couldn't open the MbimDevice: ",
				              (char const *)error->message);
				exit (EXIT_FAILURE);
			}

			mbim->_retry = 0;
			mbim->_send_request();
			Genode::log("Message returned");
		}


		static void _device_new_ready(GObject *unsused, GAsyncResult *res, gpointer user_data)
		{
			Genode::log("device: ", (void *)user_data);
			Mbim *mbim = reinterpret_cast<Mbim *>(user_data);
			GError *error = NULL;
			MbimDeviceOpenFlags open_flags = MBIM_DEVICE_OPEN_FLAGS_NONE;

			mbim->_device = mbim_device_new_finish (res, &error);
			if (!mbim->_device) {
				Genode::error("couldn't create MbimDevice: ",
				              (char const*)error->message);
				exit (EXIT_FAILURE);
			}

			mbim_device_open_full(mbim->_device,
			                      open_flags,
			                      30,
			                      nullptr,
			                      (GAsyncReadyCallback) _device_open_ready,
			                      mbim);
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

		Mbim(Libc::Env &env)
		{
			_init();
			_connect();
			exit(0);
		}
};


void Libc::Component::construct(Libc::Env &env)
{
	Libc::with_libc([&] () {
		static Mbim main { env };
	});
}
