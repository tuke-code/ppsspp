#include "Common/Log.h"
#include "Core/MemMapHelpers.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/AtracCtx2.h"
#include "Core/HW/Atrac3Standalone.h"

// Convenient command line:
// Windows\x64\debug\PPSSPPHeadless.exe  --root pspautotests/tests/../ -o --compare --timeout=30 --graphics=software pspautotests/tests/audio/atrac/... --ignore pspautotests/tests/audio/atrac/second/resetting.prx --ignore pspautotests/tests/audio/atrac/second/replay.prx
//
// See the big comment in sceAtrac.cpp for an overview of the different modes of operation.

void Atrac2::DoState(PointerWrap &p) {
	_assert_msg_(false, "Savestates not yet support with new Atrac implementation.\n\nTurn it off in Developer settings.\n\n");
}

int Atrac2::Analyze(u32 addr, u32 size) {
	return AnalyzeAtracTrack(addr, size, &track_);
}

int Atrac2::AnalyzeAA3(u32 addr, u32 size, u32 filesize) {
	return AnalyzeAA3Track(addr, size, filesize, &track_);
}

int Atrac2::RemainingFrames() const {
	const SceAtracIdInfo &info = context_->info;

	if (info.state == ATRAC_STATUS_ALL_DATA_LOADED) {
		// The buffer contains everything.
		return PSP_ATRAC_ALLDATA_IS_ON_MEMORY;
	}

	const bool isStreaming = (info.state & ATRAC_STATUS_STREAMED_MASK) != 0;

	if ((int)info.decodePos >= track_.endSample) {
		if (info.state == ATRAC_STATUS_STREAMED_WITHOUT_LOOP) {
			return PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
		}
		int loopEndAdjusted = track_.loopEndSample - track_.FirstOffsetExtra() - track_.firstSampleOffset;
		if (info.state == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER && info.decodePos > loopEndAdjusted) {
			// No longer looping in this case, outside the loop.
			return PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
		}
		if (isStreaming && loopNum_ == 0) {
			return PSP_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY;
		}
		return info.streamDataByte / info.sampleSize;
	}

	if (isStreaming) {
		// Since we're streaming, the remaining frames are what's valid in the buffer.
		return info.streamDataByte / info.sampleSize;
	}

	// Fallback
	return info.dataEnd / info.sampleSize;
}

u32 Atrac2::SecondBufferSize() const {
	const SceAtracIdInfo &info = context_->info;
	return info.secondBufferByte;
}

u32 Atrac2::AddStreamDataSas(u32 bufPtr, u32 bytesToAdd) {
	_dbg_assert_(false);
	return 0;
}

u32 Atrac2::ResetPlayPosition(int sample, int bytesWrittenFirstBuf, int bytesWrittenSecondBuf) {
	_dbg_assert_(false);
	return 0;
}

void Atrac2::GetResetBufferInfo(AtracResetBufferInfo *bufferInfo, int sample) {
	_dbg_assert_(false);
}

int Atrac2::SetLoopNum(int loopNum) {
	SceAtracIdInfo &info = context_->info;
	if (info.loopEnd <= 0) {
		return SCE_ERROR_ATRAC_NO_LOOP_INFORMATION;
	}
	info.loopNum = loopNum;
	return 0;
}

u32 Atrac2::GetNextSamples() {
	// TODO: This has to be possible to do in an easier way, from the context_->info state
	// instead of reading the track.
	int currentSample = CurrentSample();
	// It seems like the PSP aligns the sample position to 0x800...?
	u32 skipSamples = track_.FirstSampleOffsetFull();
	u32 firstSamples = (track_.SamplesPerFrame() - skipSamples) % track_.SamplesPerFrame();
	u32 numSamples = track_.endSample + 1 - currentSample;
	if (currentSample == 0 && firstSamples != 0) {
		numSamples = firstSamples;
	}

	u32 unalignedSamples = (skipSamples + currentSample) % track_.SamplesPerFrame();
	if (unalignedSamples != 0) {
		// We're off alignment, possibly due to a loop.  Force it back on.
		numSamples = track_.SamplesPerFrame() - unalignedSamples;
	}
	if (numSamples > track_.SamplesPerFrame())
		numSamples = track_.SamplesPerFrame();
	return numSamples;
}

int Atrac2::AddStreamData(u32 bytesToAdd) {
	SceAtracIdInfo &info = context_->info;
	// if (bytesToAdd > first_.writableBytes)
	//	return SCE_ERROR_ATRAC_ADD_DATA_IS_TOO_BIG;
	info.streamDataByte += bytesToAdd;
	return 0;
}

void Atrac2::GetStreamDataInfo(u32 *writePtr, u32 *writableBytes, u32 *readOffset) {
	SceAtracIdInfo &info = context_->info;
	
	*readOffset = info.curOff + info.streamDataByte;
	*writePtr = info.buffer + info.streamOff;
	// TODO: Circular buffering might make this trickier.
	*writableBytes = info.bufferByte - info.streamDataByte - info.sampleSize;
	// Probably we treat
	INFO_LOG(Log::Audio, "asdf");
}

u32 Atrac2::DecodeData(u8 *outbuf, u32 outbufPtr, u32 *SamplesNum, u32 *finish, int *remains) {
	SceAtracIdInfo &info = context_->info;

	u32 inAddr = info.buffer + info.streamOff;
	context_->codec.inBuf = inAddr;  // just because.

	/*
	const uint8_t *indata = Memory::GetPointer(inAddr);
	int bytesConsumed = 0;
	int outSamples = 0;
	if (!decoder_->Decode(indata, track_.bytesPerFrame, &bytesConsumed, outputChannels_, decodeTemp_, &outSamples)) {
		// Decode failed.
		*SamplesNum = 0;
		*finish = 1;
		// Is this the right error code? Needs testing.
		return SCE_ERROR_ATRAC_ALL_DATA_DECODED;
	}
	*/

	info.streamDataByte -= info.sampleSize;
	info.streamOff += info.sampleSize;
	info.curOff += info.sampleSize;
	info.decodePos += track_.SamplesPerFrame();

	*remains = RemainingFrames();
	return 0;
}

int Atrac2::SetData(u32 bufferAddr, u32 readSize, u32 bufferSize, int outputChannels, int successCode) {
	if (track_.codecType != PSP_MODE_AT_3 && track_.codecType != PSP_MODE_AT_3_PLUS) {
		// Shouldn't have gotten here, Analyze() checks this.
		context_->info.state = ATRAC_STATUS_NO_DATA;
		ERROR_LOG(Log::ME, "unexpected codec type %d in set data", track_.codecType);
		return SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
	}

	if (outputChannels != track_.channels) {
		// TODO: Figure out what this means
		WARN_LOG(Log::ME, "Atrac::SetData: outputChannels %d doesn't match track_.channels %d", outputChannels, track_.channels);
	}

	context_->codec.inBuf = bufferAddr;

	// Copied from the old implementation, let's see where they are useful.
	int firstExtra = track_.FirstOffsetExtra();

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
	info.curOff = track_.dataByteOffset + track_.bytesPerFrame;  // Possibly this is the result of pre-decoding the first frame already in SetData?
	info.streamOff = info.curOff;
	info.streamDataByte = readSize - info.curOff;
	info.dataEnd = track_.fileSize;
	info.decodePos = track_.FirstSampleOffsetFull();

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

	CreateDecoder();

	if (!decodeTemp_) {
		decodeTemp_ = new int16_t[track_.SamplesPerFrame() * track_.channels];
	}

	// TODO: Decode the first dummy frame to the temp buffer. This initializes the decoder.
	// It really does seem to be what's happening here, as evidenced by inBuf in the codec struct - it gets initialized.

	return successCode;
}

u32 Atrac2::SetSecondBuffer(u32 secondBuffer, u32 secondBufferSize) {
	return 0;
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
