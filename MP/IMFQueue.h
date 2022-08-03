#ifndef MF_QUEUEIMPL_H_
#define MF_QUEUEIMPL_H_

#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "MFTypes.h"

template <typename T>
class IMFQueue
{

public:

	typedef struct MF_QUEUE_INFO
	{
		std::string QueueName = "";
		time_t timeout = 100;
		size_t maxLen = -1;
		int protocol = SOCK_DGRAM;

		volatile WhoIAm whoIAm = IAmUnknown;

		//int nQueuesConnected;
		int nChannels = 1;
		//int nObjectsHave;
		//int nObjectsMax;
		//int nObjectsDropped;
		//int nObjectsFlushed;
		//int nMessagesHave;
		//int nMessagesMax;
		//int nMessagesDropped;
		//int nMessagesFlushed;
	}	MF_QUEUE_INFO;

	virtual HRESULT Place( /*[in]*/ const std::string& strQueueID, bool asServer, size_t maxLen, time_t timeout) = 0;
	virtual bool isOnline() = 0;

	virtual HRESULT Put( /*[in]*/ const std::shared_ptr<T>& pBufferOrFrame) = 0;
	virtual HRESULT Get( /*[out]*/ std::shared_ptr<T>& pBufferOrFrame, bool andPop) = 0;

	virtual HRESULT Size(size_t& size) = 0;

	virtual HRESULT Pop() = 0;
	virtual HRESULT Clear() = 0;

	virtual HRESULT Release() = 0;

	virtual ~IMFQueue() {}
};

#endif