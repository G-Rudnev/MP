#pragma warning(disable:4996)		//unsafe functions

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <queue>
#include <mutex>
#include <thread>
#include <future>

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>

#pragma comment(lib, "Ws2_32.lib")

#include "IMFQueue.h"

//#define PACKET_LEN SO_MAX_MSG_SIZE
#define PACKET_LEN 1 << 10

inline bool operator != (sockaddr_in s1, sockaddr_in s2) {
	return	s1.sin_addr.S_un.S_addr != s2.sin_addr.S_un.S_addr ||
		s1.sin_port != s2.sin_port ||
		s1.sin_family != s2.sin_family;
}

inline bool checkTimeout(const time_t& t0, const time_t& timeout) {
	if (clock() - t0 > timeout) {
		printf("Timed out.\n");
		return true;
	}
	return false;
}

template <typename T>
bool CheckSocketResult(const T& iResult, const char* errMsg, T&& errVal = SOCKET_ERROR, bool&& checkError = true) {
	if (iResult == errVal) {
		printf("%s. ", errMsg);
		if (checkError)
			printf("Error code: %d\n", WSAGetLastError());
		return true;
	}
	return false;
}

template <typename T>
struct MFQueue : public IMFQueue<T> {

	const int headerLen = sizeof(Header<T>);
	const int queryLen = sizeof(QueueQuery);

	int addressLen = sizeof(sockaddr_in);

	//Initializes the remote behaviour of the queue object - makes it online. Returns S_OK on success, otherwise returns S_FALSE.
	//If failed, closes all that it has opened.
	HRESULT Init(/*[in]*/ const std::string& strQueueID, bool asServer = false, size_t maxLen = 64, time_t timeout = 100) override {

		std::lock_guard _li(infoMutex);

		if (info.whoIAm == IAmServer) {
			printf("The queue is already in use as a server.\n");
			return S_FALSE;
		}
		else if (info.whoIAm == IAmClient) {
			printf("The queue is already in use as a client.\n");
			return S_FALSE;
		}
		else if (!mainThread.joinable()) {

			//RESET NAME
			info.QueueName = "";
			
			//PARSING ADDRESS
			if (!(info.protocol = ParseID(address, strQueueID)))
				return S_FALSE;

			info.timeout = timeout;
			info.maxLen = maxLen;

			//INIT SEREVER LOOP with the init promise
			std::promise<HRESULT> _initErrPromise;
			if (asServer)
				mainThread = std::thread(&MFQueue::ServerLoop, this, std::ref(_initErrPromise));
			else
				mainThread = std::thread(&MFQueue::ClientLoop, this, std::ref(_initErrPromise));

			if (_initErrPromise.get_future().get() != S_OK) {
				mainThread.join();
				return S_FALSE;
			}
			else {
				info.QueueName = strQueueID;
				//info.whoIAm will be define in a further loop
				return S_OK;
			}
		}
		else {
			printf("Something went wrong\n");
			return S_FALSE;
		}

	}

	bool isOnline() override {
		return _onLine;
	}

	//Sends data if this is a client connected, otherwise puts data in owned queue container
	//Returns:
	//S_OK - on success, 
	//E_BOUNDS - if queue is full,
	//S_FALSE - on error which is moderate,
	//E_ABORT - on error which is hard, so that way you have to call Close().
	//If this is not a client returns only S_OK or E_BOUNDS.
	HRESULT Put(/*[in]*/ const std::shared_ptr<T>& pData) override {

		static sockaddr_in server;
		static int recvLen, nBytes, nBytesOnce;
		
		std::lock_guard _li(infoMutex);

		if (!_onLine)
			printf("Note: the queue is not connected, call Init() method.\n");

		std::lock_guard _ld(_dataMutex);

		if (info.whoIAm == IAmClient) {

			_outputQuery.action = queueQuery_Put;
			//_outputQuery.channel = queueQuery_Ch1;
			//_outputQuery.free = 0xff;
			_outputQuery.crc = pData->data.front() + pData->data.back();
			_outputQuery.info = 0;
			_outputQuery.payload = headerLen + (int)pData->data.size();
			_outputQuery.content = typeid(T).hash_code();

			std::lock_guard _le(eventMutex);

			time_t t0 = clock();

			if (CheckSocketResult(
				send(sock, (char*)_pOutputQuery, queryLen, 0),
				"Sending query failed"))
				return E_ABORT;

			for (;;) {

				if (checkTimeout(t0, info.timeout))
					return S_FALSE;

				if ((_ev = WSAWaitForMultipleEvents(1, &_hEvent, FALSE, 1, FALSE)) == WSA_WAIT_FAILED) {
					printf("Waiting for put confirmation failed. Error code: %d\n", WSAGetLastError());
					return E_ABORT;
				}

				else if (_ev != WSA_WAIT_TIMEOUT) {

					if (WSAEnumNetworkEvents(sock, _hEvent, &_events) == SOCKET_ERROR) {
						printf("Retrieving put confirmation failed. Error code: %d\n", WSAGetLastError());
						return E_ABORT;
					}

					if (_events.lNetworkEvents & FD_READ) {

						server = address;
						recvLen = 0;
						
						//RECV QUERY
						if ((recvLen = recvfrom(sock, (char*)_pInputQuery, queryLen, 0, (sockaddr*)&server, &addressLen)) == SOCKET_ERROR) {
							printf("Receiving put confirmation failed. Error code: %d\n", WSAGetLastError());
							return E_ABORT;
						}

						if (server != address ||
							recvLen != sizeof(QueueQuery) ||
							_inputQuery.free != 0xff) 
						{
							printf("Wrong put confirmation.\n");
							return S_FALSE;
						}

						if (_inputQuery.action == queueQuery_Full) {
							printf("The queue is full.\n");
							return E_BOUNDS;
						}

						if (checkTimeout(t0, info.timeout))
							return S_FALSE;

						//SENDING HEADER
						if (CheckSocketResult(
							send(sock, (char*)&pData->header, headerLen, 0),
							"Sending header failed"))
							return E_ABORT;

						nBytes = headerLen;
						nBytesOnce = 0;

						while (nBytes < _outputQuery.payload) {

							if (checkTimeout(t0, info.timeout))
								return S_FALSE;

							if (_outputQuery.payload - nBytes >= PACKET_LEN)
								nBytesOnce = send(sock, (char*)pData->data.data(), PACKET_LEN, 0);
							else
								nBytesOnce = send(sock, (char*)pData->data.data(), _outputQuery.payload - nBytes, 0);

							if (CheckSocketResult(
								nBytesOnce,
								"Sending data failed"))
								return E_ABORT;

							nBytes += nBytesOnce;
						}

						break;
					}
				}
			}
		}

		else {
			if (_dataQueue.size() >= info.maxLen) {
				printf("The queue is full.\n");
				return E_BOUNDS;
			}
			_dataQueue.push(pData);
		}

		return S_OK;
	}

	//Queries for data to the remote server if this is a client connected, otherwise gets data from the owned queue container
	//Returns:
	//S_OK - on success, 
	//E_BOUNDS - if queue is empty,
	//S_FALSE - on error which is moderate,
	//E_ABORT - on error which is hard, so that way you have to call Close().
	//If this is not a client returns only S_OK or E_BOUNDS.
	HRESULT Get(/*[out]*/ std::shared_ptr<T>& pData, bool andPop = true) override {
		
		static sockaddr_in server;
		static int recvLen, nBytes;

		std::lock_guard _li(infoMutex);

		if (!_onLine)
			printf("Note: the queue is not connected, call Init() method.\n");

		std::lock_guard _ld(_dataMutex);

		if (info.whoIAm == IAmClient) {

			if (andPop)
				_outputQuery.action = queueQuery_Get;
			else
				_outputQuery.action = queueQuery_Peek;
			//_outputQuery.channel = queueQuery_Ch1;
			//_outputQuery.free = 0xff;
			_outputQuery.crc = 0;
			//_outputQuery.info = 0;
			_outputQuery.payload = 0;
			_outputQuery.content = typeid(T).hash_code();

			std::lock_guard _le(eventMutex);

			time_t t0 = clock();

			if (CheckSocketResult(
				send(sock, (char*)_pOutputQuery, queryLen, 0),
				"Sending query failed"))
				return E_ABORT;

			for (;;) {

				if (checkTimeout(t0, info.timeout))
					return S_FALSE;

				if ((_ev = WSAWaitForMultipleEvents(1, &_hEvent, FALSE, 1, FALSE)) == WSA_WAIT_FAILED) {
					printf("Waiting for get confirmation failed. Error code: %d\n", WSAGetLastError());
					return E_ABORT;
				}
				else if (_ev != WSA_WAIT_TIMEOUT) {

					if (WSAEnumNetworkEvents(sock, _hEvent, &_events) == SOCKET_ERROR) {
						printf("Retrieving get confirmation failed. Error code: %d\n", WSAGetLastError());
						return E_ABORT;
					}

					if (_events.lNetworkEvents & FD_READ) {

						server = address;
						recvLen = 0;

						if ((recvLen = recvfrom(sock, (char*)_pInputQuery, queryLen, 0, (sockaddr*)&server, &addressLen)) == SOCKET_ERROR) {
							printf("Receiving get confirmation failed. Error code: %d\n", WSAGetLastError());
							return E_ABORT;
						}

						if (server != address ||
							recvLen != sizeof(QueueQuery) ||
							_inputQuery.free != 0xff)
						{
							printf("Wrong get confirmation.\n");
							return S_FALSE;
						}

						if (_inputQuery.action == queueQuery_Empty) {
							printf("The queue is empty.\n");
							return E_BOUNDS;
						}

						auto elData2Put = std::make_shared<T>(T(_inputQuery.payload - headerLen));
						nBytes = 0; 

						for (;;) {

							if (checkTimeout(t0, info.timeout))
								return S_FALSE;

							//Wait for event
							if (CheckSocketResult(
								(_ev = WSAWaitForMultipleEvents(1, &_hEvent, FALSE, 1, FALSE)),
								"Waiting for data failed", WSA_WAIT_FAILED))
								return E_ABORT;

							else if (_ev != WSA_WAIT_TIMEOUT) {

								//Enumerate event
								if (CheckSocketResult(
									WSAEnumNetworkEvents(sock, _hEvent, &_events),
									"Retrieving data failed")) 
									return E_ABORT;

								if (_events.lNetworkEvents & FD_READ) {

									if (nBytes < headerLen)
										recvLen = recvfrom(sock, (char*)&elData2Put->header, headerLen, 0, (sockaddr*)&server, &addressLen);
									else
										recvLen = recvfrom(sock, (char*)elData2Put->data.data() + nBytes - headerLen, PACKET_LEN, 0, (sockaddr*)&server, (int*)&addressLen);

									if (server != address)
										continue;

									if (CheckSocketResult(
										recvLen,
										"Receieving data failed"))
										return E_ABORT;

									if ((nBytes += recvLen) < _inputQuery.payload)
										continue;
									else
										break;
								}
							}
						}

						if (_inputQuery.crc != (elData2Put->data.front() + elData2Put->data.back())) {
							printf("Wrong crc.\n");
							return S_FALSE;
						}

						pData = std::move(elData2Put);
						break;
					}
				}
			}
		}
		else {
			if (_dataQueue.size() == 0) {
				printf("The queue is empty.\n");
				return E_BOUNDS;
			}
			pData = _dataQueue.front();
			if (andPop)
				_dataQueue.pop();
		}

		return S_OK;
	}

	//Queries for the queue size.
	//Returns:
	//S_OK - on success, 
	//S_FALSE - on error which is moderate,
	//E_ABORT - on error which is hard, so that way you have to call Close().
	//If this is not a client returns only S_OK.
	HRESULT Size(size_t& size) override{
		std::lock_guard _li(infoMutex);

		if (!_onLine)
			printf("Note: the queue is not connected.\n");

		std::lock_guard _ld(_dataMutex);

		if (info.whoIAm == IAmClient) {

			_outputQuery.action = queueQuery_Size;
			//_outputQuery.channel = queueQuery_Ch1;
			//_outputQuery.free = 0xff;
			_outputQuery.crc = 0;
			_outputQuery.info = 0;
			_outputQuery.payload = 0;
			_outputQuery.content = typeid(T).hash_code();

			HRESULT ret = _SendAndRecvNoDataQuery("size");
			if (ret == S_OK)
				size = _inputQuery.info;
			return ret;
		}

		else {
			size = _dataQueue.size();

			return S_OK;
		}
	}

	//Pops the front of the queue. If this is a client, sending pop query to the remote queue server, if not - pops the owned queue.
	//Returns:
	//S_OK - on success, 
	//S_FALSE - on error which is moderate,
	//E_ABORT - on error which is hard, so that way you have to call Close().
	//If this is not a client returns only S_OK.
	HRESULT Pop() override {

		std::lock_guard _li(infoMutex);

		if (!_onLine)
			printf("Note: the queue is not connected.\n");

		std::lock_guard _ld(_dataMutex);

		if (info.whoIAm == IAmClient) {

			_outputQuery.action = queueQuery_Pop;
			//_outputQuery.channel = queueQuery_Ch1;
			//_outputQuery.free = 0xff;
			_outputQuery.crc = 0;
			//_outputQuery.info = 0;
			_outputQuery.payload = 0;
			_outputQuery.content = typeid(T).hash_code();

			return _SendAndRecvNoDataQuery("pop");
		}

		else {
			if (_dataQueue.size())
				_dataQueue.pop();

			return S_OK;
		}
	}

	//Clears the queue. If this is a client, sending clear query to the remote queue server, if not - clears the owned queue.
	//Returns:
	//S_OK - on success, 
	//S_FALSE - on error which is moderate,
	//E_ABORT - on error which is hard, so that way you have to call Close().
	//If this is not a client returns only S_OK.
	HRESULT Clear() override {

		std::lock_guard _li(infoMutex);

		if (!_onLine)
			printf("Note: the queue is not connected.\n");

		std::lock_guard _ld(_dataMutex);

		if (info.whoIAm == IAmClient) {

			_outputQuery.action = queueQuery_Clear;
			//_outputQuery.channel = queueQuery_Ch1;
			//_outputQuery.free = 0xff;
			_outputQuery.crc = 0;
			//_outputQuery.info = 0;
			_outputQuery.payload = 0;
			_outputQuery.content = typeid(T).hash_code();

			std::lock_guard _le(eventMutex);

			return  _SendAndRecvNoDataQuery("clear");
		}

		else {
			while (_dataQueue.size())
				_dataQueue.pop();
			
			return S_OK;
		}

	}

	//Closes the remote behaviour of the queue object - makes it offline.
	//Does not clear the queue.
	//Returns S_FALSE if this object is not online, otherwise returns S_OK.
	HRESULT Close() noexcept override {

		if (_onLine) {
			_onLine = false;

			mainThread.join();

			return S_OK;
		}
		else
			return S_FALSE;
	}

	//Returns protocol type as in the Winsock.h defined and 0 if an error occured.
	int ParseID(/*[in, out]*/sockaddr_in& name, const std::string& ID) {

		size_t pos1 = 0;
		size_t pos2 = ID.find("://");
		int ret = 0;

		if (ID.substr(pos1, pos2) == "TCP" || ID.substr(pos1, pos2) == "tcp" || ID.substr(pos1, pos2) == "Tcp")
			ret = SOCK_STREAM;
		else if (ID.substr(pos1, pos2) == "UDP" || ID.substr(pos1, pos2) == "udp" || ID.substr(pos1, pos2) == "Udp")
			ret = SOCK_DGRAM;

		if (!ret) {
			printf("Wrong protocol\n");
			return 0;
		}

		if (pos2 == std::string::npos) {
			printf("Wrong ID\n");
			return 0;
		}
		pos2 += 2;	//"//"

		char offset = 0;
		while (offset < 4) {
			pos1 = pos2 + 1;
			if (offset < 3)
				pos2 = ID.find(".", pos1);
			else
				pos2 = ID.find(":", pos1);
			if (pos2 <= pos1) {
				printf("Wrong ID\n");
				return 0;
			}
			int b = std::stoi(ID.substr(pos1, pos2 - pos1));
			if (b >= 1 << 8 * sizeof(UCHAR)) {
				printf("Wrong ID\n");
				return 0;
			}
			*(&name.sin_addr.S_un.S_un_b.s_b1 + offset++) = (UCHAR)b;
		}

		int port = std::stoi(ID.substr(pos2 + 1, std::string::npos));

		if (port >= 1 << 8 * sizeof(USHORT)) {
			printf("Wrong ID\n");
			return 0;
		}

		//Port
		name.sin_port = htons(port);
		name.sin_family = AF_INET;

		return ret;
	}

	~MFQueue() {
		Close();
	}

protected:

	IMFQueue<T>::MF_QUEUE_INFO info;

	sockaddr_in address;
	SOCKET sock;

	std::mutex infoMutex;
	std::mutex eventMutex;

	std::thread mainThread;

	//Being call by Init(asServer = true) in the mainThread
	void ServerLoop(std::promise<HRESULT>& initErrPromise) {

		//Initialize Winsock dll
		int iResult = WSAStartup(MAKEWORD(2, 2), &_wsaData);
		if (iResult != S_OK) {
			printf("WSAStartup failed. Error code: %d\n", iResult);
			initErrPromise.set_value(S_FALSE);
			return;
		}

		//Open socket
		sock = socket(AF_INET, info.protocol, 0);
		if (sock == INVALID_SOCKET) {
			printf("Socket opening failed. Error code: %d\n", WSAGetLastError());
			WSACleanup();
			initErrPromise.set_value(S_FALSE);
			return;
		}
		
		//Socket event
		_hEvent = WSACreateEvent();

		bool errFlag = false;

		for (;;) {

			//Setup blocking mode
			u_long nonBlocking = 1;
			if (errFlag = CheckSocketResult(
				ioctlsocket(sock, FIONBIO, &nonBlocking), 
				"Socket setup failed"))
				break;

			//Binding socket
			if (errFlag = CheckSocketResult(
				::bind(sock, (sockaddr*)&address, sizeof(address)),
				"Socket binding failed"))
				break;

			//Assign event
			if (errFlag = CheckSocketResult(
				WSAEventSelect(sock, _hEvent, FD_READ | FD_CLOSE),
				"Event assignment failed")) {
				WSACloseEvent(_hEvent);
				break;
			}

			WSAResetEvent(_hEvent);

			break;
		}

		if (errFlag) {
			closesocket(sock);
			WSACleanup();
			initErrPromise.set_value(S_FALSE);
			return;
		}
		else {
			printf("Server initialization successfull!\n");

			info.whoIAm = IAmServer;
			_onLine = true;

			initErrPromise.set_value(S_OK);
		}

		sockaddr_in client, curClient;
		int recvLen = 0;

		time_t t0;
		int nBytes, nBytesOnce;

//SERVER ONLINE LOOP
		while (_onLine) {

			if ((_ev = WSAWaitForMultipleEvents(1, &_hEvent, FALSE, 1, FALSE)) == WSA_WAIT_FAILED) {
				printf("Waiting for query failed. Error code: %d\n", WSAGetLastError());
				break;
			}
			else if (_ev != WSA_WAIT_TIMEOUT) {

				if (WSAEnumNetworkEvents(sock, _hEvent, &_events) == SOCKET_ERROR)
				{
					printf("Retrieving query failed. Error code: %d\n", WSAGetLastError());
					break;
				}

				if (_events.lNetworkEvents & FD_READ)
				{
					if ((recvLen = recvfrom(sock, (char*)_pInputQuery, queryLen, 0, (sockaddr*)&curClient, &addressLen)) == SOCKET_ERROR)
					{
						printf("Receiving query failed. Error code: %d\n", WSAGetLastError());
						break;
					}

					if (recvLen == sizeof(QueueQuery) && _inputQuery.free == 0xff) {

						std::lock_guard _ld(_dataMutex);
						
						_outputQuery.channel = _inputQuery.channel;
						_outputQuery.free = _inputQuery.free;
						_outputQuery.content = _inputQuery.content;

						t0 = clock();
						errFlag = false;

//GET or PEEK
						if (_inputQuery.action == queueQuery_Get || _inputQuery.action == queueQuery_Peek) {

							_outputQuery.info = 0;
							if (_dataQueue.size() == 0) {
								_outputQuery.action = queueQuery_Empty;
								_outputQuery.crc = 0;
								_outputQuery.payload = 0;
							}
							else {
								_outputQuery.action = queueQuery_Accept;
								_outputQuery.crc = _dataQueue.front()->data.front() + _dataQueue.front()->data.back();
								_outputQuery.payload = headerLen + (int)_dataQueue.front()->data.size();
							}

							//sending confirmation
							if (CheckSocketResult(
								sendto(sock, (char*)_pOutputQuery, queryLen, 0, (sockaddr*)&curClient, addressLen),
								"Sending get confirmation failed"))
								break;

							if (_outputQuery.action == queueQuery_Accept) {

								auto& elData2Send = _dataQueue.front();

								for (;;) {

									if (errFlag = checkTimeout(t0, info.timeout))
										break;

									//sending header
									if (errFlag = CheckSocketResult(
										sendto(sock, (char*)&elData2Send->header, headerLen, 0, (sockaddr*)&curClient, addressLen),
										"Sending header failed"))
										break;

									nBytes = headerLen;
									nBytesOnce = 0;

									while (nBytes < _outputQuery.payload) {

										if (errFlag = checkTimeout(t0, info.timeout))
											break;

										if (_outputQuery.payload - nBytes >= PACKET_LEN)
											nBytesOnce = sendto(sock, (char*)elData2Send->data.data(), PACKET_LEN, 0, (sockaddr*)&curClient, addressLen);
										else
											nBytesOnce = sendto(sock, (char*)elData2Send->data.data(), _outputQuery.payload - nBytes, 0, (sockaddr*)&curClient, addressLen);

										if (errFlag = CheckSocketResult(
											nBytesOnce,
											"Sending data failed"))
											break;

										nBytes += nBytesOnce;
									}

									break;
								}

								if (!errFlag) {
									if (_inputQuery.action == queueQuery_Get)
										_dataQueue.pop();
									printf("Data for %s:%d is sent. Queue size is %zd.\n", inet_ntoa(curClient.sin_addr), ntohs(curClient.sin_port), _dataQueue.size());
								}
							}
						}

//PUT
						else if (_inputQuery.action == queueQuery_Put) {

							_outputQuery.info = 0;
							if (_dataQueue.size() >= info.maxLen) {
								_outputQuery.action = queueQuery_Full;
								_outputQuery.crc = 0;
								_outputQuery.payload = 0;
							}
							else {
								_outputQuery.action = queueQuery_Accept;
								_outputQuery.crc = _inputQuery.crc;
								_outputQuery.payload = _inputQuery.payload;
							}

							//sending confirmation
							if (CheckSocketResult(
								sendto(sock, (char*)_pOutputQuery, queryLen, 0, (sockaddr*)&curClient, addressLen),
								"Sending put confirmation failed"))
								break;

							if (_outputQuery.action == queueQuery_Accept) {

								auto elData2Put = std::make_shared<T>(T(_inputQuery.payload - headerLen));
								nBytes = 0;

								for (;;) {

									if (errFlag = checkTimeout(t0, info.timeout))
										break;

									//Wait for event
									if (errFlag = CheckSocketResult(
										(_ev = WSAWaitForMultipleEvents(1, &_hEvent, FALSE, 1, FALSE)),
										"Waiting for data failed", WSA_WAIT_FAILED))
										break;

									else if (_ev != WSA_WAIT_TIMEOUT) {

										//Enumerate event
										if (errFlag = CheckSocketResult(
											WSAEnumNetworkEvents(sock, _hEvent, &_events),
											"Retrieving data failed"))
											break;

										if (_events.lNetworkEvents & FD_READ) {

											if (nBytes < headerLen)
												recvLen = recvfrom(sock, (char*)&elData2Put->header, headerLen, 0, (sockaddr*)&client, &addressLen);
											else
												recvLen = recvfrom(sock, (char*)elData2Put->data.data() + nBytes - headerLen, PACKET_LEN, 0, (sockaddr*)&client, &addressLen);

											if (client != curClient)
												continue;

											if (errFlag = CheckSocketResult(
												recvLen,
												"Receiving data failed"))
												break;

											if ((nBytes += recvLen) < _inputQuery.payload)
												continue;
											else
												break;
										}
									}
								}

								if ((elData2Put->data.front() + elData2Put->data.back()) != _inputQuery.crc) {
									printf("Wrong crc.\n");
									continue;
								}

								if (!errFlag) {
									_dataQueue.push(std::move(elData2Put));
									printf("Data from %s:%d is queued. Queue size is %zd.\n", inet_ntoa(curClient.sin_addr), ntohs(curClient.sin_port), _dataQueue.size());
								}
							}
						}

//CLEAR
						else if (_inputQuery.action == queueQuery_Clear) {

							_outputQuery.action = queueQuery_Accept;
							_outputQuery.crc = 0;
							_outputQuery.info = 0;
							_outputQuery.payload = 0;

							//sending confirmation
							if (CheckSocketResult(
								sendto(sock, (char*)_pOutputQuery, queryLen, 0, (sockaddr*)&curClient, addressLen),
								"Sending clear confirmation failed"))
								break;

							while (_dataQueue.size())
								_dataQueue.pop();

							printf("Queue cleared.\n");
						}

//POP
						else if (_inputQuery.action == queueQuery_Pop) {
							
							_outputQuery.action = queueQuery_Accept;
							_outputQuery.crc = 0;
							_outputQuery.info = 0;
							_outputQuery.payload = 0;

							//sending confirmation
							if (CheckSocketResult(
								sendto(sock, (char*)_pOutputQuery, queryLen, 0, (sockaddr*)&curClient, addressLen),
								"Sending pop confirmation failed"))
								break;

							if (_dataQueue.size())
								_dataQueue.pop();

							printf("Queue popped.\n");
						}

//SIZE
						else if (_inputQuery.action == queueQuery_Size) {

							_outputQuery.action = queueQuery_Accept;
							_outputQuery.crc = 0;
							_outputQuery.info = _dataQueue.size();
							_outputQuery.payload = 0;

							//sending confirmation
							if (CheckSocketResult(
								sendto(sock, (char*)_pOutputQuery, queryLen, 0, (sockaddr*)&curClient, addressLen),
								"Sending size confirmation failed"))
								break;
						}

//CONNECT
						else if (_inputQuery.action == queueQuery_Connect) {

						_outputQuery.action = queueQuery_Accept;
						_outputQuery.crc = 0;
						_outputQuery.info = 0;
						_outputQuery.payload = 0;

						//sending confirmation
						if (CheckSocketResult(
							sendto(sock, (char*)_pOutputQuery, queryLen, 0, (sockaddr*)&curClient, addressLen),
							"Sending connect confirmation failed"))
							break;

						printf("Client %s:%d connected.\n", inet_ntoa(curClient.sin_addr), ntohs(curClient.sin_port));
						}
					}
				}

				else if (_events.lNetworkEvents & FD_CLOSE)
					printf("One of the clients has disconnected.\n");
			}
		}

		std::lock_guard _li(infoMutex);

		closesocket(sock);
		WSACloseEvent(_hEvent);
		WSACleanup();

		_onLine = false;
		info.whoIAm = IAmUnknown;
	}

	//Being call by Init(asServer = false) in the mainThread
	void ClientLoop(std::promise<HRESULT>& initErrPromise) {

		//Initialize Winsock dll
		int iResult = WSAStartup(MAKEWORD(2, 2), &_wsaData);
		if (iResult != 0) {
			printf("WSAStartup failed. Error code: %d\n", iResult);
			initErrPromise.set_value(S_FALSE);
			return;
		}

		//Open socket
		sock = socket(AF_INET, info.protocol, IPPROTO_HOPOPTS);
		if (sock == INVALID_SOCKET) {
			printf("Socket opening failed. Error code: %d\n", WSAGetLastError());
			WSACleanup();
			initErrPromise.set_value(S_FALSE);
			return;
		}

		//Socket event
		_hEvent = WSACreateEvent();

		bool errFlag = false;

		for (;;) {

			//Setup blocking mode
			u_long nonBlocking = 1;
			if (errFlag = CheckSocketResult(
				ioctlsocket(sock, FIONBIO, &nonBlocking),
				"Socket setup failed"))
				break;

			//Assign event
			if (errFlag = CheckSocketResult(
				WSAEventSelect(sock, _hEvent, FD_READ | FD_CONNECT | FD_CLOSE),
				"Event assignment failed"))
				break;
			
			WSAResetEvent(_hEvent);

			//Connecting socket (non-blocking)
			iResult = connect(sock, (sockaddr*)&address, sizeof(address));
			if (iResult == SOCKET_ERROR) {
				int errCode;
				if ((errCode = WSAGetLastError()) != WSAEWOULDBLOCK) {
					printf("Socket connecting failed. Error code: %d\n", errCode);
					errFlag = true;
					break;
				}
			}

			//Waiting for the connection event
			if (errFlag = CheckSocketResult(
				(_ev = WSAWaitForMultipleEvents(1, &_hEvent, FALSE, (DWORD)info.timeout, FALSE)),
				"Waiting for the server connection failed", WSA_WAIT_FAILED))
				break;

			else if (errFlag = (_ev == WSA_WAIT_TIMEOUT)) {
				printf("Timeouted while waiting for the server response.\n");
				break;
			}

			//Check the event to be the proper connection
			if (errFlag = CheckSocketResult(
				WSAEnumNetworkEvents(sock, _hEvent, &_events),
				"Retrieving connection failed"))
				break;

			if (errFlag = (	!(_events.lNetworkEvents & FD_CONNECT) || 
					_events.iErrorCode[FD_CONNECT_BIT] != S_OK)) 
			{
					printf("Error while connecting to the server. Error code: %d\n", _events.iErrorCode[FD_CONNECT_BIT]);
					break;
			} 

//QUERY FOR LOGIC CONNECTION
			_outputQuery.action = queueQuery_Connect;
			//_outputQuery.channel = queueQuery_Ch1;
			//_outputQuery.free = 0xff;
			_outputQuery.crc = 0;
			//_outputQuery.info = 0;
			_outputQuery.payload = 0;
			_outputQuery.content = typeid(T).hash_code();

			errFlag = (_SendAndRecvNoDataQuery("connect") != S_OK);

			break;
		}

		if (errFlag) {
			WSACloseEvent(_hEvent);
			closesocket(sock);
			WSACleanup();
			initErrPromise.set_value(S_FALSE);
			return;
		}
		else {
			printf("Got connection to %s:%d.\n", inet_ntoa(address.sin_addr), ntohs(address.sin_port));

			info.whoIAm = IAmClient;
			_onLine = true;

			initErrPromise.set_value(S_OK);
		}

//CLIENT ONLINE LOOP
		while (_onLine) {

			if (eventMutex.try_lock()) {

				if ((_ev = WSAWaitForMultipleEvents(1, &_hEvent, FALSE, 0, FALSE)) == WSA_WAIT_FAILED) {
					printf("The connection lost. Error code: %d\n", WSAGetLastError());
					eventMutex.unlock();
					break;
				}
				else if (_ev != WSA_WAIT_TIMEOUT) {

					if (WSAEnumNetworkEvents(sock, _hEvent, &_events) == SOCKET_ERROR)
					{
						printf("The connection lost. Error code: %d\n", WSAGetLastError());
						eventMutex.unlock();
						break;
					}
				}

				if (_events.lNetworkEvents & FD_CLOSE) {
					printf("The connection got closed externally\n");
					eventMutex.unlock();
					break;
				}

				eventMutex.unlock();
			}

			Sleep(10);
		}

		std::lock_guard _li(infoMutex);

		closesocket(sock);
		WSACloseEvent(_hEvent);
		WSACleanup();

		_onLine = false;
		info.whoIAm = IAmUnknown;
	}

private:

	WSADATA _wsaData;	//useless, but necessary

	std::atomic<bool> _onLine = false;

	std::queue<std::shared_ptr<T>> _dataQueue;
	std::mutex _dataMutex;

	QueueQuery _inputQuery, _outputQuery;
	QueueQuery* _pInputQuery = &_inputQuery;
	QueueQuery* _pOutputQuery = &_outputQuery;

	WSAEVENT _hEvent;
	WSANETWORKEVENTS _events;
	DWORD _ev;

	//Returns:
	//S_OK - if successful, 
	//S_FALSE - if an error is moderate,
	//E_ABORT - if an error is hard.
	HRESULT _SendAndRecvNoDataQuery(const char* action) {

		static sockaddr_in server;
		static int recvLen;

		time_t t0 = clock();
		
		if (send(sock, (char*)_pOutputQuery, queryLen, 0) == SOCKET_ERROR) {
			printf("Sending %s query failed. Error code : %d\n", action, WSAGetLastError());
			return E_ABORT;
		}

		for (;;) {

			if (checkTimeout(t0, info.timeout))
				return S_FALSE;

			if ((_ev = WSAWaitForMultipleEvents(1, &_hEvent, FALSE, 1, FALSE)) == WSA_WAIT_FAILED) {
				printf("Waiting for %s confirmation failed. Error code: %d\n", action, WSAGetLastError());
				return E_ABORT;
			}
			else if (_ev != WSA_WAIT_TIMEOUT) {

				if (WSAEnumNetworkEvents(sock, _hEvent, &_events) == SOCKET_ERROR) {
					printf("Retrieving %s clear confirmation failed. Error code: %d\n", action, WSAGetLastError());
					return E_ABORT;
				}

				if (_events.lNetworkEvents & FD_READ) {

					server = address;
					recvLen = 0;

					if ((recvLen = recvfrom(sock, (char*)_pInputQuery, queryLen, 0, (sockaddr*)&server, &addressLen)) == SOCKET_ERROR) {
						printf("Receiving %s confirmation failed. Error code: %d\n", action, WSAGetLastError());
						return E_ABORT;
					}

					if (server != address ||
						recvLen != sizeof(QueueQuery) ||
						_inputQuery.free != 0xff) {
						printf("Wrong %s confirmation.\n", action);
						return S_FALSE;
					}

					if (_inputQuery.action != queueQuery_Accept) {
						printf("The %s query was not accepted by server.\n", action);
						return S_FALSE;
					}

					break;
				}
			}
		}
		return S_OK;
	}

};