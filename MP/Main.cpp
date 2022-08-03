#include "MFQueue.h"

/*
using namespace cv;

bool openCam(VideoCapture& cam, const int width, const int height) {

	auto codec = VideoWriter::fourcc('M', 'J', 'P', 'G');

	int camID;
	cin >> camID;
	cam.open(camID, CAP_ANY);

	bool camsOK = false;
	if (cam.set(CAP_PROP_FRAME_WIDTH, width))
		if (cam.set(CAP_PROP_FRAME_HEIGHT, height))
			if (cam.set(CAP_PROP_FOURCC, codec))
				camsOK = true;

	if (!camsOK || cam.get(CAP_PROP_FRAME_WIDTH) != width) {
		cerr << "Cams is not OK!" << endl;
		return false;
	}
	else
		return true;
}
*/

class r_ {

	std::mutex _m;
	double x, _;

public:

	r_() : x(sqrt(time(NULL))), _(0) {	}

	double get() {
		std::lock_guard _l(_m);
		return x = modf(33.0 * x, &_);
	}
} r;

void put_(MFQueue<MF_BUFFER>& q, const std::shared_ptr<MF_BUFFER>& pBuf) {
	
	HRESULT hRes = q.Put(pBuf);

	if (hRes == E_BOUNDS) {
		if (r.get() > 0.7)
			hRes = q.Pop();
		else
			hRes = q.Clear();
	}

	if (hRes == E_ABORT)
		q.Close();
}

void get_(MFQueue<MF_BUFFER>& q, std::shared_ptr<MF_BUFFER>& pBuf) {
	
	HRESULT hRes;
	
	if (r.get() > 0.5)
		hRes = q.Get(pBuf);
	else
		hRes = q.Get(pBuf, false);

	if (hRes == E_ABORT)
		q.Close();
}

//#define IamSERVER

int main() {

#ifdef IamSERVER
	
	MFQueue<MF_BUFFER> q, q1;
	size_t size;

	std::shared_ptr pBufOut = std::make_shared<MF_BUFFER>(MF_BUFFER(100000));
	std::shared_ptr<MF_BUFFER> pBufIn;

	std::thread thr1(&MFQueue<MF_BUFFER>::Init, std::ref(q), "udp://127.0.0.1:12345", true, 10, 100);
	Sleep(5);
	std::thread thr2(&MFQueue<MF_BUFFER>::Init, std::ref(q), "udp://127.0.0.1:12345", true, 10, 100);
	std::thread thr3(&MFQueue<MF_BUFFER>::Init, std::ref(q1), "udp://127.0.0.1:12345", true, 10, 100);

	thr1.join();
	thr2.join();
	thr3.join();

	while (q.Put(pBufOut) != E_BOUNDS) {}

	q.Size(size);
	std::cout << size << std::endl;

	q.Close();

	Sleep(100);

	q.Init("udp://127.0.0.1:12345", true, 15);
	while (q.Put(pBufOut) != E_BOUNDS) {}

	q.Size(size);
	std::cout << size << std::endl;

	clock_t T = clock() + clock_t( (r.get() + 0.2) * 100000.0 );
	while (clock() < T) {

		if (r.get() > 0.5)
			put_(q, pBufOut);
		else
			get_(q, pBufIn);

		Sleep(250);
	}

	q.Close();
	q.Clear();
	q1.Close();
	q1.Clear();

	//while (q.isOnline())
	//	Sleep(200);

#else

	MFQueue<MF_BUFFER> q, q1;

	q.Init("udp://127.0.0.1:12345");
	q1.Init("udp://127.0.0.1:12345");

	std::shared_ptr pBufOut = std::make_shared<MF_BUFFER>(MF_BUFFER(120000));
	std::shared_ptr<MF_BUFFER> pBufIn1;
	std::shared_ptr<MF_BUFFER> pBufIn2;
	
	size_t size;
	q.Size(size);
	std::cout << size << std::endl;

	int hRes = 0;

	std::thread thr1;
	std::thread thr2;

	while (q.isOnline()) {

		if (r.get() > 0.5)
			thr1 = std::thread(put_, std::ref(q), std::ref(pBufOut));
		else
			thr1 = std::thread(get_, std::ref(q), std::ref(pBufIn1));

		if (r.get() > 0.5)
			thr2 = std::thread(put_, std::ref(q1), std::ref(pBufOut));
		else
			thr2 = std::thread(get_, std::ref(q1), std::ref(pBufIn2));

		thr1.join();
		thr2.join();

		Sleep(250);
	}
#endif

	system("Pause");
	return 0;
}
