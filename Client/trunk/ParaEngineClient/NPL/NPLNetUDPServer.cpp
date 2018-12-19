#include "ParaEngine.h"
#include <boost/bind.hpp>
#include "NPLNetUDPServer.h"


namespace NPL {

	/// @def default NPL server IP or domain name
	const std::string NPL_DEFAULT_UDP_SERVER = "127.0.0.1";
	/// @def default NPL server version
	const std::string NPL_UDP_SERVER_VERSION = "0.1";
	


	CNPLNetUDPServer::CNPLNetUDPServer()
		: m_io_service_dispatcher()
		, m_route_manager()
		, m_udp(m_io_service_dispatcher)
		, m_idle_timer(m_io_service_dispatcher)
		, m_strServer(NPL_DEFAULT_UDP_SERVER)
		, m_nPort(NPL_DEFAULT_UDP_PORT)
		, m_bIsServerStarted(false)
		, m_bEnableIdleTimeout(true)
		, m_nIdleTimeoutMS(DEFAULT_IDLE_TIMEOUT_MS)
		, m_msg_dispatcher(*this, m_route_manager) // TODO: this gives a warning. find a better way to pass this pointer.
	{

	}

	CNPLNetUDPServer::~CNPLNetUDPServer()
	{
		stop();
	}

	void CNPLNetUDPServer::start(const char* server, unsigned short port)
	{
		if (m_dispatcherThread.get() != 0)
		{
			// One can only start the server once, unless we are listening to a new port
			return;
		}

		try
		{
			if (port == 0)
			{
				// start as client
				m_strServer = "";
				m_nPort = 0;

				m_udp.open(boost::asio::ip::udp::v4());
			}
			else
			{
				// start as server
				if (server != 0)
					m_strServer = server;

				m_nPort = port;
				OUTPUT_LOG("NPL UDP server %s is listening on %s:%d\n", NPL_UDP_SERVER_VERSION.c_str(), m_strServer.c_str(), m_nPort);

				boost::asio::ip::udp::endpoint local_add;
				if (server && m_strServer != "0")
				{
					local_add = boost::asio::ip::udp::endpoint(boost::asio::ip::make_address_v4(server), port);

				}
				else
				{
					local_add = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port);
				}

				m_udp.open(local_add.protocol());
				m_udp.bind(local_add);

				NPLUDPAddress_ptr address(new NPLUDPAddress(local_add, "localhost"));
				m_msg_dispatcher.AddNPLUDPAddress(address);
				address.reset(new NPLUDPAddress(local_add, "local"));
				m_msg_dispatcher.AddNPLUDPAddress(address);
				address.reset(new NPLUDPAddress(local_add, "local"));
				m_msg_dispatcher.AddNPLUDPAddress(address);
			}

			m_udp.set_option(boost::asio::socket_base::reuse_address(true));
			m_udp.set_option(boost::asio::socket_base::broadcast(true));

			m_idle_timer.expires_from_now(boost::chrono::milliseconds(GetIdleTimeoutPeriod()));
			m_idle_timer.async_wait(boost::bind(&CNPLNetUDPServer::handle_idle_timeout, this, boost::asio::placeholders::error));

			m_work_lifetime.reset(new boost::asio::io_service::work(m_io_service_dispatcher));
			m_dispatcherThread.reset(new boost::thread(boost::bind(&boost::asio::io_service::run, &m_io_service_dispatcher)));

			m_bIsServerStarted = true;

			m_udp.async_receive_from(boost::asio::buffer(m_receive_buffer), m_remote_endpoint, boost::bind(&CNPLNetUDPServer::handle_receive, this,
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred));
		}
		catch (std::exception & e)
		{
			m_udp.close();
			stop();
			OUTPUT_LOG("NPL UDP server error : %s\n", e.what());
		}
	

	}

	void CNPLNetUDPServer::broadcast(const char* buff, size_t size, NPLUDPRoute_ptr pRoute)
	{
		boost::asio::ip::udp::endpoint broadcast_ep(boost::asio::ip::address_v4::broadcast(), pRoute->GetBroadcastPort());

		m_udp.async_send_to(boost::asio::buffer(buff, size), broadcast_ep,
			boost::bind(&CNPLUDPRoute::handle_send, pRoute
				, boost::asio::placeholders::error
				, boost::asio::placeholders::bytes_transferred
				, buff
				, size));
	}

	void CNPLNetUDPServer::SendTo(const char* buff, size_t size, NPLUDPRoute_ptr pRoute)
	{
		m_udp.async_send_to(boost::asio::buffer(buff, size), pRoute->GetNPLUDPAddress()->GetEndPoint(),
			boost::bind(&CNPLUDPRoute::handle_send, pRoute
				, boost::asio::placeholders::error
				, boost::asio::placeholders::bytes_transferred
				, buff
				, size));
	}

	void CNPLNetUDPServer::SendTo(const char* buff, size_t size, NPLUDPAddress_ptr pAddress)
	{
		SendTo(buff, size, pAddress->GetEndPoint());
	}


	void CNPLNetUDPServer::SendTo(const char* buff, size_t size, const boost::asio::ip::udp::endpoint& ep)
	{
		m_udp.async_send_to(boost::asio::buffer(buff, size), ep,
			boost::bind(&CNPLNetUDPServer::handle_send, this
				, boost::asio::placeholders::error
				, boost::asio::placeholders::bytes_transferred
				, buff
				, size));
	}

	void CNPLNetUDPServer::SendTo(const char* buff, size_t size, const std::string& remoteIp, unsigned short remotePort)
	{
		boost::asio::ip::udp::endpoint remote_endpoint(boost::asio::ip::make_address_v4(remoteIp), remotePort);
		SendTo(buff, size, remote_endpoint);
	}

	void CNPLNetUDPServer::stop()
	{
		if (m_dispatcherThread.get() != 0)
		{
			m_bIsServerStarted = false;

			// cancel timer
			m_idle_timer.cancel();

			// Post a call to the stop function so that server::stop() is safe to call
			// from any thread.
			m_io_service_dispatcher.post(boost::bind(&CNPLNetUDPServer::handle_stop, this));

			// stop the work on dispatcher. 
			m_work_lifetime.reset();

			m_dispatcherThread->join();
			m_dispatcherThread.reset();

			Cleanup();
			m_io_service_dispatcher.reset();
		}
	}



	NPLUDPRoute_ptr CNPLNetUDPServer::CreateRoute(NPLUDPAddress_ptr pAddress)
	{
		NPLUDPRoute_ptr pRoute(new CNPLUDPRoute(*this, m_route_manager, m_msg_dispatcher));
		pRoute->SetNPLUDPAddress(pAddress);
		m_route_manager.start(pRoute);
		return pRoute;
	}

	void CNPLNetUDPServer::handle_stop()
	{
		m_route_manager.stop_all();
	}


	void CNPLNetUDPServer::Cleanup()
	{
		m_msg_dispatcher.Cleanup();
	}

	boost::asio::io_context& CNPLNetUDPServer::GetIoService()
	{
		return m_io_service_dispatcher;
	}

	void CNPLNetUDPServer::handle_receive(const boost::system::error_code& error, std::size_t bytes_transferred)
	{
		
		auto route = m_route_manager.GetRoute(m_remote_endpoint);

		if (!route.get())
		{
			route.reset(new CNPLUDPRoute(*this, m_route_manager, m_msg_dispatcher));

			// this function is assumed to be called only in the dispatcher thread, so no lock is needed to generate the temp id. 
			char tmp[32];
			itoa(m_remote_endpoint.port(), tmp, 10);
			string nid = "~udp" + m_remote_endpoint.address().to_string();
			nid.append("_");
			nid.append(tmp);

			NPLUDPAddress_ptr pAddress(new NPLUDPAddress(m_remote_endpoint, nid));
			route->SetNPLUDPAddress(pAddress);
			m_msg_dispatcher.AddNPLUDPAddress(pAddress);
			m_route_manager.start(route);
		}
	
		auto bRes = route->handleReceivedData(m_receive_buffer.data(), bytes_transferred);

		if (!bRes)
		{
			m_route_manager.stop(route);
		}

		m_udp.async_receive_from(boost::asio::buffer(m_receive_buffer), m_remote_endpoint, boost::bind(&CNPLNetUDPServer::handle_receive, this,
			boost::asio::placeholders::error,
			boost::asio::placeholders::bytes_transferred));
	}

	void CNPLNetUDPServer::handle_send(const boost::system::error_code& error, size_t bytes_transferred, const char* buff, size_t buff_size)
	{

	}

	/** Handle idle timer timeout.*/
	void CNPLNetUDPServer::handle_idle_timeout(const boost::system::error_code& err)
	{
		if (!err)
		{
			if (IsIdleTimeoutEnabled())
			{
				m_route_manager.CheckIdleTimeout();
			}

			// continue with next activation. 
			m_idle_timer.expires_from_now(boost::chrono::milliseconds(IDLE_TIMEOUT_TIMER_INTERVAL)); // GetIdleTimeoutPeriod()
			m_idle_timer.async_wait(boost::bind(&CNPLNetUDPServer::handle_idle_timeout, this, boost::asio::placeholders::error));
		}
	}


	void CNPLNetUDPServer::SetIdleTimeoutPeriod(int nMilliseconds)
	{
		m_nIdleTimeoutMS = nMilliseconds;
		if (m_nIdleTimeoutMS <= 0) {
			OUTPUT_LOG("warning: IdleTimeoutPeriod is 0, we will disable idle time out, otherwise connection will be closed immediately.\n");
			m_bEnableIdleTimeout = false;
		}
	}

	int CNPLNetUDPServer::GetIdleTimeoutPeriod()
	{
		return m_nIdleTimeoutMS;
	}

	void CNPLNetUDPServer::EnableIdleTimeout(bool bEnable)
	{
		m_bEnableIdleTimeout = bEnable;
	}

	bool CNPLNetUDPServer::IsIdleTimeoutEnabled()
	{
		return m_bEnableIdleTimeout;
	}

	const std::string& CNPLNetUDPServer::GetHostIP()
	{
		return m_strServer;
	}

	unsigned short CNPLNetUDPServer::GetHostPort()
	{
		return  m_nPort;
	}

	bool CNPLNetUDPServer::IsServerStarted()
	{
		return m_bIsServerStarted;
	}

} // namespace NPL