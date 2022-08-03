#ifndef MF_TYPES_H_
#define MF_TYPES_H_

#ifndef S_OK

typedef	enum HRESULT
{
	S_OK = 0,
	S_FALSE = 1,
	E_ABORT = 0x80004004L,
	E_NOTIMPL = 0x80004001L,
	E_BOUNDS = 0x8000000BL,
	E_INVALIDARG = 0x80070057L
} HRESULT;

#endif

template <class Container>
struct Header { };

template <class Container>
struct MF_BASE_DATATYPE
{
	MF_BASE_DATATYPE() = default;

	explicit MF_BASE_DATATYPE(size_t dataLen) : data(Container(dataLen)) {	}
	explicit MF_BASE_DATATYPE(int dataLen) : data(Container(dataLen)) {	}

	virtual ~MF_BASE_DATATYPE() {}

	Header<Container> header;
	Container data;
};



typedef std::vector<uint8_t> Buffer;

typedef enum eMFBufferFlags
{
	eMFBF_Empty = 0,
	eMFBF_Buffer = 0x1,
	eMFBF_Packet = 0x2,
	eMFBF_Frame = 0x3,
	eMFBF_Stream = 0x4,
	eMFBF_SideData = 0x10,
	eMFBF_VideoData = 0x20,
	eMFBF_AudioData = 0x40,
} 	eMFBufferFlags;

template<>
struct Header<Buffer> {
	eMFBufferFlags       flags = eMFBF_Empty;
};

typedef struct MF_BUFFER : public MF_BASE_DATATYPE<Buffer>
{
	using MF_BASE_DATATYPE::MF_BASE_DATATYPE;

	typedef std::shared_ptr<MF_BUFFER> TPtr;

} MF_BUFFER;

enum WhoIAm {
	IAmUnknown,
	IAmServer,
	IAmClient
};

enum QueueQueryType
{
	queueQuery_Ignore,
	queueQuery_Connect,
	queueQuery_Put,
	queueQuery_Get,
	queueQuery_Peek,
	queueQuery_Size,
	queueQuery_Pop,
	queueQuery_Clear,
	queueQuery_Accept,
	queueQuery_Full,
	queueQuery_Empty,
};

enum QueueQueryChannel
{
	queueQuery_Ch1,
	queueQuery_Ch2,
};

typedef struct QueueQuery
{
	UCHAR action = queueQuery_Ignore;
	UCHAR channel = queueQuery_Ch1;
	UCHAR free = 0xff;
	UCHAR crc = 0;
	size_t info = 0;
	int payload = 0;
	size_t content = 0;
} QueueQuery;
#endif
