/*
 * OpenFrotz runtime hooks for ScummVM Glk.
 */

#include "engines/glk/openfrotz_runtime_hooks.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

namespace Glk {
namespace OpenFrotzRuntimeHooks {
namespace {
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_active = false;
Common::String g_storyPath;
std::deque<Common::String> g_commands;
Common::String g_transcript;
uint64 g_revision = 0;
constexpr size_t kMaxTranscriptBytes = 256 * 1024;

void bumpRevisionLocked() {
	g_revision++;
	g_cv.notify_all();
}

void appendRawLocked(const Common::String &text) {
	if (text.empty()) {
		return;
	}
	g_transcript += text;
	if (g_transcript.size() > kMaxTranscriptBytes) {
		const size_t drop = g_transcript.size() - kMaxTranscriptBytes;
		g_transcript = Common::String(g_transcript.c_str() + drop);
	}
	bumpRevisionLocked();
}
} // namespace

void startSession(const Common::String &storyPath) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_active = true;
	g_storyPath = storyPath;
	g_commands.clear();
	g_transcript.clear();
	appendRawLocked("ScummVM OpenFrotz runtime hooks active.\n");
	appendRawLocked("Story: " + storyPath + "\n");
}

void endSession() {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_active = false;
	g_storyPath.clear();
	g_commands.clear();
	g_transcript.clear();
	bumpRevisionLocked();
}

bool isActive() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_active;
}

void enqueueCommand(const Common::String &command) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_active) {
		return;
	}
	g_commands.push_back(command);
	bumpRevisionLocked();
}

bool dequeueCommand(Common::String &command) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_active || g_commands.empty()) {
		return false;
	}
	command = g_commands.front();
	g_commands.pop_front();
	return true;
}

bool hasQueuedCommand() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_active && !g_commands.empty();
}

void appendOutput(const char *buf, uint len) {
	if (!buf || len == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_active) {
		return;
	}
	appendRawLocked(Common::String(buf, buf + len));
}

void appendOutputUni(const uint32 *buf, uint len) {
	if (!buf || len == 0) {
		return;
	}
	Common::String ascii;
	ascii.reserve(len);
	for (uint i = 0; i < len; i++) {
		const uint32 ch = buf[i];
		if (ch >= 32 && ch <= 126) {
			ascii += static_cast<char>(ch);
		} else if (ch == '\n' || ch == '\r' || ch == '\t') {
			ascii += static_cast<char>(ch);
		} else {
			ascii += '?';
		}
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_active) {
		return;
	}
	appendRawLocked(ascii);
}

void appendInputEcho(const Common::String &line) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_active) {
		return;
	}
	appendRawLocked("> " + line + "\n");
}

Common::String waitForTranscriptSince(uint64 afterRevision, uint32 timeoutMs, uint64 *outRevision) {
	std::unique_lock<std::mutex> lock(g_mutex);
	if (!g_active) {
		if (outRevision) {
			*outRevision = g_revision;
		}
		return "";
	}
	if (g_revision <= afterRevision) {
		g_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
			return g_revision > afterRevision || !g_active;
		});
	}
	if (outRevision) {
		*outRevision = g_revision;
	}
	return g_transcript;
}

Common::String getTranscriptSnapshot(uint64 *outRevision) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (outRevision) {
		*outRevision = g_revision;
	}
	return g_transcript;
}

} // namespace OpenFrotzRuntimeHooks
} // namespace Glk
