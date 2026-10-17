#include "flash/obs_link.h"

#include <windows.h>

#include <cstring>

namespace flash::obslink {

std::vector<uint8_t> pack(uint8_t type, const void *data, size_t size)
{
	const uint32_t len = (uint32_t)size + 1;
	std::vector<uint8_t> out(4 + len);
	std::memcpy(out.data(), &len, 4);
	out[4] = type;
	if (size)
		std::memcpy(out.data() + 5, data, size);
	return out;
}

std::vector<uint8_t> pack(uint8_t type, const std::string &json)
{
	return pack(type, json.data(), json.size());
}

Link::Link(void *pipe, void *stop)
	: pipe_(pipe), stop_(stop), read_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
	  write_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

Link::~Link()
{
	CloseHandle(read_event_);
	CloseHandle(write_event_);
}

// Une lecture ou une écriture complète, abandonnée si le tube se ferme ou si l'arrêt est demandé
bool Link::io(bool reading, void *buf, uint32_t size)
{
	uint8_t *p = (uint8_t *)buf;
	while (size > 0) {
		OVERLAPPED ov{};
		ov.hEvent = reading ? read_event_ : write_event_;
		ResetEvent(ov.hEvent);
		DWORD done = 0;
		const BOOL ok = reading ? ReadFile(pipe_, p, size, nullptr, &ov) : WriteFile(pipe_, p, size, nullptr, &ov);
		if (!ok && GetLastError() != ERROR_IO_PENDING)
			return false;
		HANDLE waits[2] = {ov.hEvent, stop_};
		if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
			CancelIoEx(pipe_, &ov);
			GetOverlappedResult(pipe_, &ov, &done, TRUE);
			return false;
		}
		if (!GetOverlappedResult(pipe_, &ov, &done, FALSE) || done == 0)
			return false;
		p += done;
		size -= done;
	}
	return true;
}

bool Link::read(Message &m)
{
	uint32_t len = 0;
	if (!io(true, &len, 4) || len == 0 || len > (64u << 20))
		return false;
	std::vector<uint8_t> buf(len);
	if (!io(true, buf.data(), len))
		return false;
	m.type = buf[0];
	m.body.assign(buf.begin() + 1, buf.end());
	return true;
}

bool Link::write(const std::vector<uint8_t> &bytes)
{
	return io(false, (void *)bytes.data(), (uint32_t)bytes.size());
}

} // namespace flash::obslink
