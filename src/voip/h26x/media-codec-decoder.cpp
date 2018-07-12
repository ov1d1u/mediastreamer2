/*
 Mediastreamer2 media-codec-decoder.cpp
 Copyright (C) 2018 Belledonne Communications SARL

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include <jni.h>
#include <media/NdkMediaFormat.h>
#include <ortp/b64.h>
#include <ortp/str_utils.h>

#include "mediastreamer2/formats.h"
#include "mediastreamer2/msfilter.h"
#include "mediastreamer2/msticker.h"

#include "android_mediacodec.h"
#include "h26x-utils.h"
#include "media-codec-decoder.h"

using namespace b64;
using namespace mediastreamer;
using namespace std;

namespace mediastreamer {

MediaCodecDecoder::MediaCodecDecoder(const std::string &mime) {
	try {
		_impl = AMediaCodec_createDecoderByType(mime.c_str());
		if (_impl == nullptr) {
			ostringstream msg;
			msg << "could not create MediaCodec for '" << mime << "'";
			throw runtime_error(msg.str());
		}
		_format = createFormat(mime);
		_bufAllocator = ms_yuv_buf_allocator_new();
		_naluHeader.reset(H26xToolFactory::get(mime).createNaluHeader());
		startImpl();
	} catch (const runtime_error &e) {
		if (_impl) AMediaCodec_delete(_impl);
		if (_format) AMediaFormat_delete(_format);
		if (_bufAllocator) ms_yuv_buf_allocator_free(_bufAllocator);
		throw e;
	}
}

MediaCodecDecoder::~MediaCodecDecoder() {
	AMediaCodec_delete(_impl);
	ms_yuv_buf_allocator_free(_bufAllocator);
}

void MediaCodecDecoder::setParameterSets(MSQueue *paramterSets, uint64_t timestamp) {
	if (!feed(paramterSets, timestamp, true)) {
		ms_error("MSMediaCodecH264Dec: paramter sets has been refused by the decoder.");
		return;
	}
	_needParameters = false;
}

bool MediaCodecDecoder::feed(MSQueue *encodedFrame, uint64_t timestamp) {
	bool status = false;
	if (_needParameters) {
		ms_error("MSMediaCodecH264Dec: waiting for paramter sets.");
		goto clean;
	}

	if (_needKeyFrame) {
		if (!isKeyFrame(encodedFrame)) {
			ms_error("MSMediaCodecH264Dec: waiting for key frame.");
			goto clean;
		}
		_needKeyFrame = false;
	}

	if (!feed(encodedFrame, timestamp, false)) {
		goto clean;
	}

	_pendingFrames++;
	status = true;

clean:
	ms_queue_flush(encodedFrame);
	return status;
}

mblk_t *MediaCodecDecoder::fetch() {
	mblk_t *om = nullptr;
	AMediaImage image = {0};
	int dst_pix_strides[4] = {1, 1, 1, 1};
	MSRect dst_roi = {0};
	AMediaCodecBufferInfo info;
	ssize_t oBufidx = -1;

	if (_impl == nullptr || _pendingFrames <= 0) goto end;

	oBufidx = AMediaCodec_dequeueOutputBuffer(_impl, &info, _timeoutUs);
	if (oBufidx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
		ms_message("MSMediaCodecH264Dec: output format has changed.");
		oBufidx = AMediaCodec_dequeueOutputBuffer(_impl, &info, _timeoutUs);
	}

	if (oBufidx < 0) {
		if (oBufidx == AMEDIA_ERROR_UNKNOWN) {
			ms_error("MSMediaCodecH264Dec: AMediaCodec_dequeueOutputBuffer() had an exception");
		} else if (oBufidx != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
			ms_error("MSMediaCodecH264Dec: unknown error while dequeueing an output buffer (oBufidx=%zd)", oBufidx);
		}
		goto end;
	}

	_pendingFrames--;

	if (AMediaCodec_getOutputImage(_impl, oBufidx, &image) <= 0) {
		ms_error("AMediaCodec_getOutputImage() failed");
		goto end;
	}

	MSPicture pic;
	om = ms_yuv_buf_allocator_get(_bufAllocator, &pic, image.crop_rect.w, image.crop_rect.h);
	ms_yuv_buf_copy_with_pix_strides(image.buffers, image.row_strides, image.pixel_strides, image.crop_rect,
										pic.planes, pic.strides, dst_pix_strides, dst_roi);
	AMediaImage_close(&image);

end:
	if (oBufidx >= 0) AMediaCodec_releaseOutputBuffer(_impl, oBufidx, FALSE);
	return om;
}

AMediaFormat *MediaCodecDecoder::createFormat(const std::string &mime) const {
	AMediaFormat *format = AMediaFormat_new();
	AMediaFormat_setString(format, "mime", mime.c_str());
	AMediaFormat_setInt32(format, "color-format", 0x7f420888);
	AMediaFormat_setInt32(format, "max-width", 1920);
	AMediaFormat_setInt32(format, "max-height", 1920);
	AMediaFormat_setInt32(format, "priority", 0);
	return format;
}

void MediaCodecDecoder::startImpl() {
	media_status_t status = AMEDIA_OK;
	ostringstream errMsg;
	if ((status = AMediaCodec_configure(_impl, _format, nullptr, nullptr, 0)) != AMEDIA_OK) {
		errMsg << "configuration failure: " << int(status);
		throw runtime_error(errMsg.str());
	}

	if ((status = AMediaCodec_start(_impl)) != AMEDIA_OK) {
		errMsg << "starting failure: " << int(status);
		throw runtime_error(errMsg.str());
	}
}

void MediaCodecDecoder::stopImpl() {
	AMediaCodec_stop(_impl);
}

bool MediaCodecDecoder::feed(MSQueue *encodedFrame, uint64_t timestamp, bool isPs) {
	H26xUtils::nalusToByteStream(encodedFrame, _bitstream);

	if (_impl == nullptr) return false;

	ssize_t iBufidx = AMediaCodec_dequeueInputBuffer(_impl, _timeoutUs);
	if (iBufidx < 0) {
		ms_error("MSMediaCodecH264Dec: %s.", iBufidx == -1 ? "no buffer available for queuing this frame ! Decoder is too slow" : "AMediaCodec_dequeueInputBuffer() had an exception");
		return false;
	}

	size_t bufsize;
	uint8_t *buf = AMediaCodec_getInputBuffer(_impl, iBufidx, &bufsize);
	if (buf == nullptr) {
		ms_error("MSMediaCodecH264Dec: AMediaCodec_getInputBuffer() returned NULL");
		return false;
	}

	size_t size = _bitstream.size();
	if (size > bufsize) {
		ms_error("Cannot copy the all the bitstream into the input buffer size : %zu and bufsize %zu", size, bufsize);
		size = min(size, bufsize);
	}
	memcpy(buf, _bitstream.data(), size);

	uint32_t flags = isPs ? BufferFlag::CodecConfig : BufferFlag::None;
	if (AMediaCodec_queueInputBuffer(_impl, iBufidx, 0, size, timestamp * 1000ULL, flags) != 0) {
		ms_error("MSMediaCodecH264Dec: AMediaCodec_queueInputBuffer() had an exception");
		return false;
	}

	return true;
}

bool MediaCodecDecoder::isKeyFrame(const MSQueue *frame) const {
	for (const mblk_t *nalu = ms_queue_peek_first(frame); !ms_queue_end(frame, nalu); nalu = ms_queue_next(frame, nalu)) {
		_naluHeader->parse(nalu->b_rptr);
		if (_naluHeader->getAbsType().isKeyFramePart()) return true;
	}
	return false;
}

MediaCodecDecoderFilterImpl::MediaCodecDecoderFilterImpl(MSFilter *f, const std::string &mime):
	_vsize({0, 0}),
	_f(f),
	_unpacker(H26xToolFactory::get(mime).createNalUnpacker()),
	_psStore(H26xToolFactory::get(mime).createParameterSetsStore()),
	_naluHeader(H26xToolFactory::get(mime).createNaluHeader()) {

	try {
		_codec.reset(new MediaCodecDecoder(mime));
		ms_message("MSMediaCodecH264Dec initialization");
		ms_average_fps_init(&_fps, " H264 decoder: FPS: %f");
	} catch (const runtime_error &e) {
		ms_error("MSMediaCodecH264Dec: %s", e.what());
		_codec.reset(nullptr);
	}
}

void MediaCodecDecoderFilterImpl::preprocess() {
	_firstImageDecoded = false;
	if (_codec) _codec->waitForKeyFrame();
}

void MediaCodecDecoderFilterImpl::process() {
	bool requestPli = false;
	MSQueue frame, parameterSets;

	if (_codec == nullptr) {
		ms_queue_flush(_f->inputs[0]);
		return;
	}

	ms_queue_init(&frame);
	ms_queue_init(&parameterSets);

	while (mblk_t *im = ms_queue_get(_f->inputs[0])) {
		NalUnpacker::Status unpacking_ret = _unpacker->unpack(im, &frame);

		if (!unpacking_ret.frameAvailable) continue;

		if (unpacking_ret.frameCorrupted) {
			ms_warning("MSMediaCodecH264Dec: corrupted frame");
			requestPli = true;
			if (_freezeOnError) {
				ms_queue_flush(&frame);
				_codec->waitForKeyFrame();
				continue;
			}
		}

		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint64_t tsMs = (ts.tv_nsec / 1000000ULL) + 10ULL;

		extractParameterSets(&frame, &parameterSets);
		if (!ms_queue_empty(&parameterSets)) _codec->setParameterSets(&parameterSets, tsMs);

		if (!ms_queue_empty(&frame)) {
			requestPli = !_codec->feed(&frame, tsMs);
		}

		ms_queue_flush(&frame);
		ms_queue_flush(&parameterSets);
	}

	mblk_t *om;
	while ((om = _codec->fetch()) != nullptr) {
		MSPicture pic;
		ms_yuv_buf_init_from_mblk(&pic, om);
		_vsize.width = pic.w;
		_vsize.height = pic.h;

		if (!_firstImageDecoded) {
			ms_message("First frame decoded %ix%i", _vsize.width, _vsize.height);
			_firstImageDecoded = true;
			ms_filter_notify_no_arg(_f, MS_VIDEO_DECODER_FIRST_IMAGE_DECODED);
		}

		ms_average_fps_update(&_fps, _f->ticker->time);
		ms_queue_put(_f->outputs[0], om);
	}

	if (_avpfEnabled && requestPli) {
		ms_filter_notify_no_arg(_f, MS_VIDEO_DECODER_SEND_PLI);
	}
}

void MediaCodecDecoderFilterImpl::postprocess() {
	_unpacker->reset();
}

void MediaCodecDecoderFilterImpl::resetFirstImage() {
	_firstImageDecoded = false;
}

void MediaCodecDecoderFilterImpl::extractParameterSets(MSQueue *frame, MSQueue *parameterSets) {
	for (mblk_t *nalu = ms_queue_peek_first(frame); !ms_queue_end(frame, nalu);) {
		_naluHeader->parse(nalu->b_rptr);
		if (_naluHeader->getAbsType().isParameterSet()) {
			mblk_t *ps = nalu;
			nalu = ms_queue_next(frame, nalu);
			ms_queue_remove(frame, ps);
			ms_queue_put(parameterSets, ps);
			continue;
		}
		nalu = ms_queue_next(frame, nalu);
	}
}

MSVideoSize MediaCodecDecoderFilterImpl::getVideoSize() const {
	return _firstImageDecoded ? _vsize : MS_VIDEO_SIZE_UNKNOWN;
}

float MediaCodecDecoderFilterImpl::getFps() const {
	return ms_average_fps_get(&_fps);
}

const MSFmtDescriptor *MediaCodecDecoderFilterImpl::getOutFmt() const {
	return ms_factory_get_video_format(_f->factory, "YUV420P", ms_video_size_make(_vsize.width, _vsize.height), 0, nullptr);
}

void MediaCodecDecoderFilterImpl::enableAvpf(bool enable) {
	_avpfEnabled = enable;
}

void MediaCodecDecoderFilterImpl::enableFreezeOnError(bool enable) {
	_freezeOnError = enable;
	ms_message("MSMediaCodecH264Dec: freeze on error %s", _freezeOnError ? "enabled" : "disabled");
}

} // namespace mediastreamer
