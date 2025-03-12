#include "Common/Log.h"
#include "Core/MemMapHelpers.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/AtracCtx2.h"
#include "Core/HW/Atrac3Standalone.h"

// Convenient command line:
// Windows\x64\debug\PPSSPPHeadless.exe  --root pspautotests/tests/../ -o --compare --new-atrac --timeout=30 --graphics=software pspautotests/tests/audio/atrac/stream.prx
//
// See the big comment in sceAtrac.cpp for an overview of the different modes of operation.
//
// Tests left to fix:
// - resetpos
// - resetting
// - second/resetting
// - second/setbuffer
// - decode
// - getremainframe  (requires seek)

// To run on the real PSP, without gentest.py:
// cmd1> C:\dev\ppsspp\pspautotests\tests\audio\atrac> make
// cmd2> C:\dev\ppsspp\pspautotests> usbhostfs_pc -b 4000
// cmd3> C:\dev\ppsspp\pspautotests> pspsh -p 4000
// cmd3> > cd host0:/test/audio/atrac
// cmd3> stream.prx
// cmd1> C:\dev\ppsspp\pspautotests\tests\audio\atrac>copy /Y ..\..\..\__testoutput.txt stream.expected
// Then run the test, see above.

// Needs to support negative numbers, and to handle non-powers-of-two.
static int RoundDownToMultiple(int x, int n) {
	return (x % n == 0) ? x : x - (x % n) - (n * (x < 0));
}

void Atrac2::DoState(PointerWrap &p) {
	_assert_msg_(false, "Savestates not yet support with new Atrac implementation.\n\nTurn it off in Developer settings.\n\n");
}

void Atrac2::AnalyzeReset() {
	track_ = {};
	track_.AnalyzeReset();

	if (context_.IsValid()) {
		SceAtracIdInfo &info = context_->info;
		info = {};
		info.state = ATRAC_STATUS_NO_DATA;
	}
}

int Atrac2::Analyze(u32 addr, u32 size) {
	AnalyzeReset();
	int retval = AnalyzeAtracTrack(addr, size, &track_);
	if (retval < 0) {
		return retval;
	}
	track_.DebugLog();
	return 0;
}

int Atrac2::AnalyzeAA3(u32 addr, u32 size, u32 filesize) {
	AnalyzeReset();
	return AnalyzeAA3Track(addr, size, filesize, &track_);
}

int Atrac2::RemainingFrames() const {
	const SceAtracIdInfo &info = context_->info;

	// Handle the easy cases first.
	switch (info.state) {
	case ATRAC_STATUS_NO_DATA:
	case ATRAC_STATUS_ALL_DATA_LOADED:
		return PSP_ATRAC_ALLDATA_IS_ON_MEMORY;  // Not sure about no data.
	case ATRAC_STATUS_HALFWAY_BUFFER:
	{
		const int fileOffset = info.streamDataByte + info.dataOff;
		if (fileOffset >= info.dataEnd) {
			return PSP_ATRAC_ALLDATA_IS_ON_MEMORY;
		}
		return (fileOffset - info.curOff) / info.sampleSize;
	}
	case ATRAC_STATUS_STREAMED_LOOP_FROM_END:
	case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER:
	case ATRAC_STATUS_STREAMED_WITHOUT_LOOP:
		// Below logic.
		break;
	default:
		return SCE_ERROR_ATRAC_BAD_ATRACID;
	}

	const int fileOffset = (int)info.curOff + (int)info.streamDataByte;
	const int bytesLeft = (int)info.dataEnd - fileOffset;
	if (bytesLeft == 0 && info.state == ATRAC_STATUS_STREAMED_WITHOUT_LOOP) {
		return PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
	}

	if ((int)info.decodePos >= track_.endSample) {
		if (info.state == ATRAC_STATUS_STREAMED_WITHOUT_LOOP) {
			return PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
		}
		int loopEndAdjusted = track_.loopEndSample - track_.FirstOffsetExtra() - track_.firstSampleOffset;
		if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER && info.decodePos > loopEndAdjusted) {
			// No longer looping in this case, outside the loop.
			return PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
		}
		if (info.loopNum == 0) {
			return PSP_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY;
		}
		return info.streamDataByte / info.sampleSize;
	}

	// Since we're streaming, the remaining frames are what's valid in the buffer.
	return info.streamDataByte / info.sampleSize;
}

bool Atrac2::HasSecondBuffer() const {
	const SceAtracIdInfo &info = context_->info;
	return info.secondBufferByte != 0;
}

u32 Atrac2::ResetPlayPosition(int sample, int bytesWrittenFirstBuf, int bytesWrittenSecondBuf) {
	// Redo the same calculation as before, for input validation.
	AtracResetBufferInfo bufferInfo;
	GetResetBufferInfo(&bufferInfo, sample);

	// Input validation.
	if ((u32)bytesWrittenFirstBuf < bufferInfo.first.minWriteBytes || (u32)bytesWrittenFirstBuf > bufferInfo.first.writableBytes) {
		return hleLogError(Log::ME, SCE_ERROR_ATRAC_BAD_FIRST_RESET_SIZE, "first byte count not in valid range");
	}
	if ((u32)bytesWrittenSecondBuf < bufferInfo.second.minWriteBytes || (u32)bytesWrittenSecondBuf > bufferInfo.second.writableBytes) {
		return hleLogError(Log::ME, SCE_ERROR_ATRAC_BAD_SECOND_RESET_SIZE, "second byte count not in valid range");
	}

	SceAtracIdInfo &info = context_->info;
	if (info.state == ATRAC_STATUS_ALL_DATA_LOADED) {
		// Always adds zero bytes.
		// But we still need to reinit the context with the sample offset (but keeping the streamDataByte size).
		InitContext(0, info.buffer, info.bufferByte, info.bufferByte, sample);
	} else if (info.state == ATRAC_STATUS_HALFWAY_BUFFER) {
		// Just reinitialize the context at the start.
		_dbg_assert_(info.dataOff + info.streamDataByte == bufferInfo.first.filePos);
		int readSize = info.dataOff + info.streamDataByte + bytesWrittenFirstBuf;
		InitContext(0, info.buffer, readSize, info.bufferByte, sample);
		if (readSize == info.dataEnd) {
			// Now, it's possible we'll transition to a full buffer here, if all the bytes were written. Let's do it.
			info.state = ATRAC_STATUS_ALL_DATA_LOADED;
		}
	} else {
		if (bufferInfo.first.filePos > track_.fileSize) {
			return hleDelayResult(hleLogError(Log::ME, SCE_ERROR_ATRAC_API_FAIL, "invalid file position"), "reset play pos", 200);
		}
		InitContext((bufferInfo.first.writePosPtr - info.buffer) + info.dataOff, info.buffer, bytesWrittenFirstBuf, info.bufferByte, sample);
	}

	return hleNoLog(0);
}

// This is basically sceAtracGetBufferInfoForResetting.
void Atrac2::GetResetBufferInfo(AtracResetBufferInfo *bufferInfo, int sample) {
	// This was mostly copied straight from the old impl.

	const SceAtracIdInfo &info = context_->info;
	if (info.state == ATRAC_STATUS_ALL_DATA_LOADED) {
		bufferInfo->first.writePosPtr = info.buffer;
		// Everything is loaded, so nothing needs to be read.
		bufferInfo->first.writableBytes = 0;
		bufferInfo->first.minWriteBytes = 0;
		bufferInfo->first.filePos = 0;
	} else if (info.state == ATRAC_STATUS_HALFWAY_BUFFER) {
		// The old logic here was busted. This, instead, appears to just replicate GetStreamDataInfo.
		GetStreamDataInfo(&bufferInfo->first.writePosPtr, &bufferInfo->first.writableBytes, &bufferInfo->first.filePos);
		bufferInfo->first.minWriteBytes = 0;
	} else {
		// This is without the sample offset.  The file offset also includes the previous batch of samples?
		int sampleFileOffset = track_.FileOffsetBySample(sample - track_.firstSampleOffset - track_.SamplesPerFrame());

		// Update the writable bytes.  When streaming, this is just the number of bytes until the end.
		const u32 bufSizeAligned = RoundDownToMultiple(info.bufferByte, track_.bytesPerFrame);
		const int needsMoreFrames = track_.FirstOffsetExtra();  // ?

		bufferInfo->first.writePosPtr = info.buffer;
		bufferInfo->first.writableBytes = std::min(track_.fileSize - sampleFileOffset, bufSizeAligned);
		if (((sample + track_.firstSampleOffset) % (int)track_.SamplesPerFrame()) >= (int)track_.SamplesPerFrame() - needsMoreFrames) {
			// Not clear why, but it seems it wants a bit extra in case the sample is late?
			bufferInfo->first.minWriteBytes = track_.bytesPerFrame * 3;
		} else {
			bufferInfo->first.minWriteBytes = track_.bytesPerFrame * 2;
		}
		if ((u32)sample < (u32)track_.firstSampleOffset && sampleFileOffset != track_.dataByteOffset) {
			sampleFileOffset -= track_.bytesPerFrame;
		}
		bufferInfo->first.filePos = sampleFileOffset;

		if (info.secondBufferByte != 0) {
			// TODO: We have a second buffer.  Within it, minWriteBytes should be zero.
			// The filePos should be after the end of the second buffer (or zero.)
			// We actually need to ensure we READ from the second buffer before implementing that.
		}
	}

	// Reset never needs a second buffer write, since the loop is in a fixed place.
	// It seems like second.writePosPtr is always the same as the first buffer's pos, weirdly (doesn't make sense).
	bufferInfo->second.writePosPtr = info.buffer;
	bufferInfo->second.writableBytes = 0;
	bufferInfo->second.minWriteBytes = 0;
	bufferInfo->second.filePos = 0;
}

int Atrac2::SetLoopNum(int loopNum) {
	SceAtracIdInfo &info = context_->info;
	if (info.loopEnd <= 0) {
		return SCE_ERROR_ATRAC_NO_LOOP_INFORMATION;
	}
	info.loopNum = loopNum;
	return 0;
}

int Atrac2::LoopNum() const {
	return context_->info.loopNum;
}

int Atrac2::LoopStatus() const {
	// Seems to be 1 while looping, until the last finish where it flips to 0.
	// I can't find this represented in the state. Well actually, maybe it's just status, the loop variants of streams?
	return context_->info.loopEnd > 0;
}

u32 Atrac2::GetNextSamples() {
	SceAtracIdInfo &info = context_->info;
	int samplesToWrite = track_.SamplesPerFrame();
	int sampleRemainder = info.decodePos % track_.SamplesPerFrame();

	// TODO: Handle end-of-track short block.
	return samplesToWrite - sampleRemainder;
}

int Atrac2::AddStreamData(u32 bytesToAdd) {
	SceAtracIdInfo &info = context_->info;

	// WARNING: bytesToAdd might not be sampleSize aligned, even though we gave it an aligned value
	// int GetStreamDataInfo, so other parts of the code still has to handle unaligned data amounts.
	if (info.state == ATRAC_STATUS_HALFWAY_BUFFER) {
		const int newFileOffset = info.streamDataByte + info.dataOff + bytesToAdd;
		if (newFileOffset == info.dataEnd) {
			info.state = ATRAC_STATUS_ALL_DATA_LOADED;
		} else if (newFileOffset > info.dataEnd) {
			return SCE_ERROR_ATRAC_ADD_DATA_IS_TOO_BIG;
		}
		info.streamDataByte += bytesToAdd;
	} else {
		info.streamDataByte += bytesToAdd;
	}
	return 0;
}

u32 Atrac2::AddStreamDataSas(u32 bufPtr, u32 bytesToAdd) {
	// Internal API, seems like a combination of GetStreamDataInfo and AddStreamData, for use when
	// an Atrac context is bound to an sceSas channel.
	// Sol Trigger is the only game I know that uses this.
	_dbg_assert_(false);
	return 0;
}

void Atrac2::GetStreamDataInfo(u32 *writePtr, u32 *bytesToRead, u32 *readFileOffset) {
	const SceAtracIdInfo &info = context_->info;

	switch (info.state) {
	case ATRAC_STATUS_ALL_DATA_LOADED:
		// Nothing to do, the whole track is loaded already.
		*writePtr = info.buffer;
		*bytesToRead = 0;
		*readFileOffset = 0;
		break;

	case ATRAC_STATUS_HALFWAY_BUFFER:
	{
		// This is both the file offset and the offset in the buffer, since it's direct mapped
		// in this mode (no wrapping or any other trickery).
		const int fileOffset = (int)info.dataOff + (int)info.streamDataByte;
		const int bytesLeftInFile = (int)info.dataEnd - fileOffset;

		if (bytesLeftInFile == 0) {
			// We've got all the data, no more loading is needed.
			// Signalling this by setting everything to default.
			*writePtr = info.buffer;
			*bytesToRead = 0;
			*readFileOffset = 0;
			return;
		}

		// Just ask for the rest of the data. The game can supply as much of it as it wants at a time.
		*writePtr = info.buffer + fileOffset;
		*readFileOffset = fileOffset;
		*bytesToRead = bytesLeftInFile;
		break;
	}

	default:
	{
		// Streaming
		//
		// This really is the core logic of sceAtrac. It looks simple, and is pretty simple, but figuring it out
		// from just logs of variables wasn't all that easy... Initially I had it more complicated, but boiled it
		// all down to fairly simple logic. And then boiled it down further and fixed bugs.
		//
		// TODO: Take care of loop points.

		const int fileOffset = (int)info.curOff + (int)info.streamDataByte;
		const int bytesLeftInFile = (int)info.dataEnd - fileOffset;

		_dbg_assert_(bytesLeftInFile >= 0);

		if (bytesLeftInFile == 0) {
			// We've got all the data up to the end buffered, no more streaming is needed.
			// Signalling this by setting everything to default.
			*writePtr = info.buffer;
			*bytesToRead = 0;
			*readFileOffset = 0;
			return;
		}

		// NOTE: writePos might not actually be packet aligned!
		// However, we can rely on being packet aligned at streamOff.
		int distanceToEnd = RoundDownToMultiple(info.bufferByte - info.streamOff, info.sampleSize);
		if (info.streamDataByte < distanceToEnd) {
			// There's space left without wrapping.
			const int writeOffset = info.streamOff + info.streamDataByte;
			*writePtr = info.buffer + writeOffset;
			*bytesToRead = std::min(distanceToEnd - (int)info.streamDataByte, bytesLeftInFile);
			*readFileOffset = *bytesToRead == 0 ? 0 : fileOffset;  // Seems this behavior (which isn't important) only happens on this path?
		} else {
			// Wraps around.
			const int firstPart = distanceToEnd;
			const int secondPart = info.streamDataByte - firstPart;
			const int spaceLeft = info.streamOff - secondPart;
			*writePtr = info.buffer + secondPart;
			*bytesToRead = std::min(spaceLeft, bytesLeftInFile);
			*readFileOffset = fileOffset;
		}
		break;
	}
	}
}

int Atrac2::CurrentSample() const {
	const SceAtracIdInfo &info = context_->info;
	return info.decodePos - track_.FirstSampleOffsetFull();
}

u32 Atrac2::DecodeData(u8 *outbuf, u32 outbufPtr, u32 *SamplesNum, u32 *finish, int *remains) {
	SceAtracIdInfo &info = context_->info;

	if (info.decodePos >= info.endSample) {
		//_dbg_assert_(info.curOff >= info.dataEnd);
		ERROR_LOG(Log::ME, "DecodeData: Reached the end, nothing to decode");
		*finish = 1;
		return SCE_ERROR_ATRAC_ALL_DATA_DECODED;
	}

	int samplesToWrite = track_.SamplesPerFrame();
	int decodePosAdvance = samplesToWrite;

	// Handle mid-buffer seeks by discarding samples.
	int sampleRemainder = info.decodePos % track_.SamplesPerFrame();
	if (sampleRemainder != 0) {
		int discardedSamples = sampleRemainder;
		_dbg_assert_(samplesToWrite >= discardedSamples);
		samplesToWrite -= discardedSamples;
		info.decodePos -= sampleRemainder;
	}

	// Try to handle the end, too.
	// NOTE: This should match GetNextSamples().
	if (info.decodePos + sampleRemainder + samplesToWrite > info.endSample + 1) {
		int samples = info.endSample + 1 - info.decodePos;
		if (samples < track_.SamplesPerFrame()) {
			samplesToWrite = samples;
			decodePosAdvance = samples;
		} else {
			ERROR_LOG(Log::ME, "Too many samples left: %08x", samples);
		}
	}

	// Check that there's enough data to decode.
	if (AtracStatusIsStreaming(info.state)) {
		if (info.streamDataByte < track_.bytesPerFrame) {
			// Seems some games actually check for this in order to refill, instead of relying on remainFrame. Pretty dumb. See #5564
			ERROR_LOG(Log::ME, "Half-way: Ran out of data to decode from");
			return SCE_ERROR_ATRAC_BUFFER_IS_EMPTY;
		}
	} else if (info.state == ATRAC_STATUS_HALFWAY_BUFFER) {
		const int fileOffset = info.streamDataByte + info.dataOff;
		if (info.curOff + track_.bytesPerFrame > fileOffset) {
			ERROR_LOG(Log::ME, "Half-way: Ran out of data to decode from");
			return SCE_ERROR_ATRAC_BUFFER_IS_EMPTY;
		}
	}

	u32 inAddr;
	switch (info.state) {
	case ATRAC_STATUS_ALL_DATA_LOADED:
		inAddr = info.buffer + info.curOff;
		break;
	default:
		inAddr = info.buffer + info.streamOff;
		break;
	}

	context_->codec.inBuf = inAddr;  // just because.
	int bytesConsumed = 0;
	int outSamples = 0;
	_dbg_assert_(decodeTemp_[track_.SamplesPerFrame() * outputChannels_] == 1337);  // Sentinel
	if (!decoder_->Decode(Memory::GetPointer(inAddr), track_.bytesPerFrame, &bytesConsumed, outputChannels_, decodeTemp_, &outSamples)) {
		// Decode failed.
		*SamplesNum = 0;
		*finish = 0;
		context_->codec.err = 0x20b;  // checked on hardware for 0xFF corruption. it's possible that there are more codes.
		return SCE_ERROR_ATRAC_API_FAIL;  // tested.
	}
	if (bytesConsumed != info.sampleSize) {
		WARN_LOG(Log::ME, "bytesConsumed mismatch: %d vs %d", bytesConsumed, info.sampleSize);
	}
	_dbg_assert_(decodeTemp_[track_.SamplesPerFrame() * outputChannels_] == 1337);  // Sentinel

	// Write the decoded samples to memory.
	// TODO: We can detect cases where we can safely just decode directly into output (full samplesToWrite, outbuf != nullptr)
	if (outbuf) {
		_dbg_assert_(samplesToWrite <= track_.SamplesPerFrame());
		memcpy(outbuf, decodeTemp_, samplesToWrite * outputChannels_ * sizeof(int16_t));
	}

	if (AtracStatusIsStreaming(info.state)) {
		info.streamDataByte -= info.sampleSize;
		info.streamOff += info.sampleSize;
	}
	info.curOff += info.sampleSize;
	info.decodePos += decodePosAdvance;

	int retval = 0;
	// detect the end.
	if (info.decodePos >= info.endSample) {
		*finish = 1;
	}
	// If we reached the end of the buffer, move the cursor back to the start.
	// SetData takes care of any split packet on the first lap (On other laps, no split packets
	// happen).
	if (AtracStatusIsStreaming(info.state) && info.streamOff + info.sampleSize > info.bufferByte) {
		INFO_LOG(Log::ME, "Hit the stream buffer wrap point (decoding).");
		info.streamOff = 0;
	}

	*SamplesNum = samplesToWrite;
	*remains = RemainingFrames();

	context_->codec.err = 0;
	return retval;
}

int Atrac2::SetData(u32 bufferAddr, u32 readSize, u32 bufferSize, int outputChannels, int successCode) {
	SceAtracIdInfo &info = context_->info;

	if (track_.codecType != PSP_MODE_AT_3 && track_.codecType != PSP_MODE_AT_3_PLUS) {
		// Shouldn't have gotten here, Analyze() checks this.
		ERROR_LOG(Log::ME, "unexpected codec type %d in set data", track_.codecType);
		return SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
	}

	if (outputChannels != track_.channels) {
		INFO_LOG(Log::ME, "Atrac::SetData: outputChannels %d doesn't match track_.channels %d, decoder will expand.", outputChannels, track_.channels);
	}

	outputChannels_ = outputChannels;

	CreateDecoder();

	if (!decodeTemp_) {
		_dbg_assert_(track_.channels <= 2);
		decodeTemp_ = new int16_t[track_.SamplesPerFrame() * outputChannels + 1];
		decodeTemp_[track_.SamplesPerFrame() * outputChannels] = 1337;  // Sentinel
	}

	context_->codec.inBuf = bufferAddr;

	if (readSize > track_.fileSize) {
		WARN_LOG(Log::ME, "readSize %d > track_.fileSize", readSize, track_.fileSize);
		readSize = track_.fileSize;
	}

	if (bufferSize >= track_.fileSize) {
		// Buffer is big enough to fit the whole track.
		if (readSize < bufferSize) {
			info.state = ATRAC_STATUS_HALFWAY_BUFFER;
		} else {
			info.state = ATRAC_STATUS_ALL_DATA_LOADED;
		}
	} else {
		// Streaming cases with various looping types.
		if (track_.loopEndSample <= 0) {
			// There's no looping, but we need to stream the data in our buffer.
			info.state = ATRAC_STATUS_STREAMED_WITHOUT_LOOP;
		} else if (track_.loopEndSample == track_.endSample + track_.FirstSampleOffsetFull()) {
			info.state = ATRAC_STATUS_STREAMED_LOOP_FROM_END;
		} else {
			info.state = ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER;
		}
	}

	INFO_LOG(Log::ME, "Atrac streaming mode setup: %s", AtracStatusToString(info.state));

	InitContext(0, bufferAddr, readSize, bufferSize, 0);
	return successCode;
}

void Atrac2::InitContext(int offset, u32 bufferAddr, u32 readSize, u32 bufferSize, int sampleOffset) {
	SceAtracIdInfo &info = context_->info;
	// Copy parameters into struct.
	info.buffer = bufferAddr;
	info.bufferByte = bufferSize;
	info.samplesPerChan = track_.FirstSampleOffsetFull();
	info.endSample = track_.endSample + info.samplesPerChan;
	if (track_.loopStartSample != 0xFFFFFFFF) {
		info.loopStart = track_.loopStartSample;
		info.loopEnd = track_.loopEndSample;
	}
	info.codec = track_.codecType;
	info.sampleSize = track_.bytesPerFrame;
	info.numChan = track_.channels;
	info.numFrame = 0;
	info.dataOff = track_.dataByteOffset;
	info.curOff = track_.dataByteOffset + ((sampleOffset + track_.FirstOffsetExtra()) / track_.SamplesPerFrame()) * info.sampleSize;  // Note: This and streamOff get incremented by bytesPerFrame before the return from this function by skipping frames.
	info.streamOff = track_.dataByteOffset - offset;
	if (AtracStatusIsStreaming(info.state)) {
		info.streamDataByte = readSize - info.streamOff;
	} else {
		info.streamDataByte = readSize - info.dataOff;
	}
	info.dataEnd = track_.fileSize;
	info.decodePos = track_.FirstSampleOffsetFull() + sampleOffset;

	int discardedSamples = track_.FirstSampleOffsetFull();

	// TODO: Decode/discard any first dummy frames to the temp buffer. This initializes the decoder.
	// It really does seem to be what's happening here, as evidenced by inBuf in the codec struct - it gets initialized.
	// Alternatively, the dummy frame is just there to leave space for wrapping...
	while (discardedSamples >= track_.SamplesPerFrame()) {
		int bytesConsumed = info.sampleSize;
		int outSamples;
		if (!decoder_->Decode(Memory::GetPointer(info.buffer + info.streamOff), info.sampleSize, &bytesConsumed, outputChannels_, decodeTemp_, &outSamples)) {
			ERROR_LOG(Log::ME, "Error decoding the 'dummy' buffer at offset %d in the buffer", info.streamOff);
		}
		outSamples = track_.SamplesPerFrame();
		if (bytesConsumed != info.sampleSize) {
			WARN_LOG(Log::ME, "bytesConsumed mismatch: %d vs %d", bytesConsumed, info.sampleSize);
		}
		_dbg_assert_(decodeTemp_[track_.SamplesPerFrame() * outputChannels_] == 1337);  // Sentinel

		info.curOff += track_.bytesPerFrame;
		if (AtracStatusIsStreaming(info.state)) {
			info.streamOff += track_.bytesPerFrame;
			info.streamDataByte -= info.sampleSize;
		}
		discardedSamples -= outSamples;
	}

	// We need to handle wrapping the overshot partial packet at the end.
	if (AtracStatusIsStreaming(info.state)) {
		// This logic is similar to GetStreamDataInfo.
		int distanceToEnd = RoundDownToMultiple(info.bufferByte - info.streamOff, info.sampleSize);
		if (info.streamDataByte < distanceToEnd) {
			// There's space left without wrapping. Don't do anything.
			INFO_LOG(Log::ME, "Streaming: Packets fit into the buffer fully. %08x < %08x", readSize, bufferSize);
			// In this case, seems we need to zero some bytes. In one test, this seems to be 336.
			// Maybe there's a logical bug and the copy happens even when not needed, it's just that it'll
			// copy zeroes. Either way, let's just copy some bytes to make our sanity check hexdump pass.
			Memory::Memset(info.buffer, 0, 128);
		} else {
			// Wraps around.
			const int copyStart = info.streamOff + distanceToEnd;
			const int copyLen = info.bufferByte - copyStart;
			// Then, let's copy it.
			INFO_LOG(Log::ME, "Streaming: Packets didn't fit evenly. Last packet got split into %d/%d (sum=%d). Copying to start of buffer.",
				copyLen, info.sampleSize - copyLen, info.sampleSize);
			Memory::Memcpy(info.buffer, info.buffer + copyStart, copyLen);
		}
	}
}

u32 Atrac2::SetSecondBuffer(u32 secondBuffer, u32 secondBufferSize) {
	return 0;
}

u32 Atrac2::GetInternalCodecError() const {
	if (context_.IsValid()) {
		return context_->codec.err;
	} else {
		return 0;
	}
}

void Atrac2::InitLowLevel(u32 paramsAddr, bool jointStereo, int atracID) {
	track_.AnalyzeReset();
	track_.channels = Memory::Read_U32(paramsAddr);
	outputChannels_ = Memory::Read_U32(paramsAddr + 4);
	track_.bytesPerFrame = Memory::Read_U32(paramsAddr + 8);
	if (track_.codecType == PSP_MODE_AT_3) {
		track_.bitrate = (track_.bytesPerFrame * 352800) / 1000;
		track_.bitrate = (track_.bitrate + 511) >> 10;
		track_.jointStereo = false;
	} else if (track_.codecType == PSP_MODE_AT_3_PLUS) {
		track_.bitrate = (track_.bytesPerFrame * 352800) / 1000;
		track_.bitrate = ((track_.bitrate >> 11) + 8) & 0xFFFFFFF0;
		track_.jointStereo = false;
	}
	track_.dataByteOffset = 0;

	EnsureContext(atracID);
	context_->info.decodePos = 0;
	context_->info.state = ATRAC_STATUS_LOW_LEVEL;
	CreateDecoder();
}
