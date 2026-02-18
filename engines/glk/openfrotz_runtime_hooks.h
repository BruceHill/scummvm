/*
 * OpenFrotz runtime hooks for ScummVM Glk.
 */

#ifndef GLK_OPENFROTZ_RUNTIME_HOOKS_H
#define GLK_OPENFROTZ_RUNTIME_HOOKS_H

#include "common/str.h"
#include "common/scummsys.h"

namespace Glk {
namespace OpenFrotzRuntimeHooks {

void startSession(const Common::String &storyPath);
void endSession();
bool isActive();

void enqueueCommand(const Common::String &command);
bool dequeueCommand(Common::String &command);
bool hasQueuedCommand();
void noteLineRequest(const char *kind, uint maxlen, uint initlen);
void noteInputEvent(const char *event, uint32 a0, uint32 a1, uint32 a2);
bool nudgeInputPump();

void appendOutput(const char *buf, uint len);
void appendOutputUni(const uint32 *buf, uint len);
void appendInputEcho(const Common::String &line);
void appendSystemMessage(const Common::String &line);

Common::String waitForTranscriptSince(uint64 afterRevision, uint32 timeoutMs, uint64 *outRevision);
bool waitForDequeueSince(uint64 afterDequeueRevision, uint32 timeoutMs, uint64 *outDequeueRevision);
Common::String waitForOutputSince(uint64 afterOutputRevision, uint32 timeoutMs, uint64 *outOutputRevision);
Common::String getTranscriptSnapshot(uint64 *outRevision);
uint64 getDequeueRevision();
uint64 getOutputRevision();

} // namespace OpenFrotzRuntimeHooks
} // namespace Glk

#endif // GLK_OPENFROTZ_RUNTIME_HOOKS_H
