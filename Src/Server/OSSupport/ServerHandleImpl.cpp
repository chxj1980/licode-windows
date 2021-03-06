
// ServerHandleImpl.cpp

// Implements the cServerHandleImpl class implementing the TCP server functionality

#include "stdafx.h"
#include "ServerHandleImpl.h"
#include "TCPLinkImpl.h"
#include "NetworkSingleton.h"





////////////////////////////////////////////////////////////////////////////////
// Globals:

static bool IsValidSocket(evutil_socket_t a_Socket)
{
	#ifdef _WIN32
		return (a_Socket != INVALID_SOCKET);
	#else  // _WIN32
		return (a_Socket >= 0);
	#endif  // else _WIN32
}





////////////////////////////////////////////////////////////////////////////////
// cServerHandleImpl:

cServerHandleImpl::cServerHandleImpl(cNetwork::cListenCallbacksPtr a_ListenCallbacks):
	m_ListenCallbacks(a_ListenCallbacks),
	m_ConnListener(nullptr),
	m_SecondaryConnListener(nullptr),
	m_IsListening(false),
	m_ErrorCode(0)
{
}





cServerHandleImpl::~cServerHandleImpl()
{
	if (m_ConnListener != nullptr)
	{
		evconnlistener_free(m_ConnListener);
	}
	if (m_SecondaryConnListener != nullptr)
	{
		evconnlistener_free(m_SecondaryConnListener);
	}
}





void cServerHandleImpl::Close(void)
{
	// Stop the listener sockets:
	evconnlistener_disable(m_ConnListener);
	if (m_SecondaryConnListener != nullptr)
	{
		evconnlistener_disable(m_SecondaryConnListener);
	}
	m_IsListening = false;

	// Shutdown all connections:
	cTCPLinkImplPtrs Conns;
	{
		cCSLock Lock(m_CS);
		std::swap(Conns, m_Connections);
	}
	for (auto conn: Conns)
	{
		conn->Shutdown();
	}

	// Remove the ptr to self, so that the object may be freed:
	m_SelfPtr.reset();

	// Remove self from cNetworkSingleton:
	cNetworkSingleton::Get().RemoveServer(this);
}





cServerHandleImplPtr cServerHandleImpl::Listen(
	UInt16 a_Port,
	cNetwork::cListenCallbacksPtr a_ListenCallbacks
)
{
	cServerHandleImplPtr res = cServerHandleImplPtr{new cServerHandleImpl(a_ListenCallbacks)};
	res->m_SelfPtr = res;
	if (res->Listen(a_Port))
	{
		cNetworkSingleton::Get().AddServer(res);
	}
	else
	{
		a_ListenCallbacks->OnError(res->m_ErrorCode, res->m_ErrorMsg);
		res->m_SelfPtr.reset();
	}
	return res;
}





bool cServerHandleImpl::Listen(UInt16 a_Port)
{
	// Make sure the cNetwork internals are innitialized:
	cNetworkSingleton::Get();

	// Set up the main socket:
	// It should listen on IPv6 with IPv4 fallback, when available; IPv4 when IPv6 is not available.
	bool NeedsTwoSockets = false;
	int err;
	evutil_socket_t MainSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (!IsValidSocket(MainSock))
	{
		m_ErrorCode = EVUTIL_SOCKET_ERROR();
		Printf(m_ErrorMsg, "Cannot create socket for port %d: %s", a_Port, evutil_socket_error_to_string(m_ErrorCode));
		return false;
	}

	// Allow the port to be reused right after the socket closes:
	if (evutil_make_listen_socket_reuseable(MainSock) != 0)
	{
		m_ErrorCode = EVUTIL_SOCKET_ERROR();
		Printf(m_ErrorMsg, "Port %d cannot be made reusable: %d (%s). Restarting the server might not work.",
			a_Port, m_ErrorCode, evutil_socket_error_to_string(m_ErrorCode)
		);
		LOG("%s", m_ErrorMsg.c_str());
	}

	// Bind to all interfaces:
	sockaddr_in name;
	memset(&name, 0, sizeof(name));
	name.sin_family = AF_INET;
	name.sin_port = ntohs(a_Port);
	if (bind(MainSock, reinterpret_cast<const sockaddr *>(&name), sizeof(name)) != 0)
	{
		m_ErrorCode = EVUTIL_SOCKET_ERROR();
		Printf(m_ErrorMsg, "Cannot bind IPv4 socket to port %d: %s", a_Port, evutil_socket_error_to_string(m_ErrorCode));
		evutil_closesocket(MainSock);
		return false;
	}

	if (evutil_make_socket_nonblocking(MainSock) != 0)
	{
		m_ErrorCode = EVUTIL_SOCKET_ERROR();
		Printf(m_ErrorMsg, "Cannot make socket on port %d non-blocking: %d (%s)", a_Port, m_ErrorCode, evutil_socket_error_to_string(m_ErrorCode));
		evutil_closesocket(MainSock);
		return false;
	}
	if (listen(MainSock, 0) != 0)
	{
		m_ErrorCode = EVUTIL_SOCKET_ERROR();
		Printf(m_ErrorMsg, "Cannot listen on port %d: %d (%s)", a_Port, m_ErrorCode, evutil_socket_error_to_string(m_ErrorCode));
		evutil_closesocket(MainSock);
		return false;
	}
	m_ConnListener = evconnlistener_new(cNetworkSingleton::Get().GetEventBase(), Callback, this, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, 0, MainSock);
	m_IsListening = true;

	if (!NeedsTwoSockets)
	{
		return true;
}

	return true;
}





void cServerHandleImpl::Callback(evconnlistener * a_Listener, evutil_socket_t a_Socket, sockaddr * a_Addr, int a_Len, void * a_Self)
{
	// Cast to true self:
	cServerHandleImpl * Self = reinterpret_cast<cServerHandleImpl *>(a_Self);
	ASSERT(Self != nullptr);
	ASSERT(Self->m_SelfPtr != nullptr);

	// Get the textual IP address and port number out of a_Addr:
	char IPAddress[128];
	UInt16 Port = 0;
	switch (a_Addr->sa_family)
	{
		case AF_INET:
		{
			sockaddr_in * sin = reinterpret_cast<sockaddr_in *>(a_Addr);
			evutil_inet_ntop(AF_INET, &(sin->sin_addr), IPAddress, ARRAYCOUNT(IPAddress));
			Port = ntohs(sin->sin_port);
			break;
		}
		case AF_INET6:
		{
			sockaddr_in6 * sin6 = reinterpret_cast<sockaddr_in6 *>(a_Addr);
			evutil_inet_ntop(AF_INET6, &(sin6->sin6_addr), IPAddress, ARRAYCOUNT(IPAddress));
			Port = ntohs(sin6->sin6_port);
			break;
		}
	}

	// Call the OnIncomingConnection callback to get the link callbacks to use:
	cTCPLink::cCallbacksPtr LinkCallbacks = Self->m_ListenCallbacks->OnIncomingConnection(IPAddress, Port);
	if (LinkCallbacks == nullptr)
	{
		// Drop the connection:
		evutil_closesocket(a_Socket);
		return;
	}

	// Create a new cTCPLink for the incoming connection:
	cTCPLinkImplPtr Link = std::make_shared<cTCPLinkImpl>(a_Socket, LinkCallbacks, Self->m_SelfPtr, a_Addr, static_cast<socklen_t>(a_Len));
	{
		cCSLock Lock(Self->m_CS);
		Self->m_Connections.push_back(Link);
	}  // Lock(m_CS)
	LinkCallbacks->OnLinkCreated(Link);
	Link->Enable(Link);

	// Call the OnAccepted callback:
	Self->m_ListenCallbacks->OnAccepted(*Link);
}





void cServerHandleImpl::RemoveLink(const cTCPLinkImpl * a_Link)
{
	cCSLock Lock(m_CS);
	for (auto itr = m_Connections.begin(), end = m_Connections.end(); itr != end; ++itr)
	{
		if (itr->get() == a_Link)
		{
			m_Connections.erase(itr);
			return;
		}
	}  // for itr - m_Connections[]
}





////////////////////////////////////////////////////////////////////////////////
// cNetwork API:

cServerHandlePtr cNetwork::Listen(
	UInt16 a_Port,
	cNetwork::cListenCallbacksPtr a_ListenCallbacks
)
{
	return cServerHandleImpl::Listen(a_Port, a_ListenCallbacks);
}





