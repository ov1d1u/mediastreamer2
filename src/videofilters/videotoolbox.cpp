/*
 mediastreamer2 library - modular sound and video processing and streaming
 Copyright (C) 2016  Belledonne Communications SARL

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

#include "filter-wrapper/encoding-filter-wrapper.h"
#include "filter-wrapper/decoding-filter-wrapper.h"
#include "h26x/h26x-decoder-filter.h"
#include "h26x/h26x-encoder-filter.h"
#include "h26x/videotoolbox-decoder.h"
#include "h26x/videotoolbox-encoder.h"

using namespace std;

namespace mediastreamer {

const MSVideoConfiguration vth264enc_video_confs[] = {
	MS_VIDEO_CONF(1536000,  2560000, SXGA_MINUS, 25, 2),
	MS_VIDEO_CONF( 800000,  2000000,       720P, 25, 2),
	MS_VIDEO_CONF( 800000,  1536000,        XGA, 25, 2),
	MS_VIDEO_CONF( 600000,  1024000,       SVGA, 25, 2),
	MS_VIDEO_CONF( 800000,  3000000,        VGA, 30, 2),
	MS_VIDEO_CONF( 400000,   800000,        VGA, 15, 1),
	MS_VIDEO_CONF( 200000,   350000,        CIF, 18, 1),
	MS_VIDEO_CONF( 150000,   200000,       QVGA, 15, 1),
	MS_VIDEO_CONF( 100000,   150000,       QVGA, 10, 1),
	MS_VIDEO_CONF(  64000,   100000,       QCIF, 12, 1),
	MS_VIDEO_CONF(      0,    64000,       QCIF,  5 ,1)
};

class VideoToolboxH264EncoderFilterImpl: public H26xEncoderFilter {
public:
	VideoToolboxH264EncoderFilterImpl(MSFilter *f): H26xEncoderFilter(f, new VideoToolboxEncoder("video/avc"), vth264enc_video_confs) {}
};

class VideoToolboxH264DecoderFilterImpl: public H26xDecoderFilter {
public:
	VideoToolboxH264DecoderFilterImpl(MSFilter *f): H26xDecoderFilter(f, new VideoToolboxDecoder("video/avc")) {}
};

} // namespace mediastreamer

using namespace mediastreamer;

MS_ENCODING_FILTER_WRAPPER_METHODS_DECLARATION(VideoToolboxH264Encoder);
MS_ENCODING_FILTER_WRAPPER_DESCRIPTION_DECLARATION(VideoToolboxH264Encoder, MS_VT_H264_ENC_ID, "H264 hardware encoder for iOS and MacOSX", "H264", MS_FILTER_IS_PUMP);

MS_DECODING_FILTER_WRAPPER_METHODS_DECLARATION(VideoToolboxH264Decoder);
MS_DECODING_FILTER_WRAPPER_DESCRIPTION_DECLARATION(VideoToolboxH264Decoder, MS_VT_H264_DEC_ID, "H264 hardware decoder for iOS and MacOSX", "H264", MS_FILTER_IS_PUMP);

extern "C" void _register_videotoolbox_if_supported(MSFactory *factory) {
#if TARGET_OS_SIMULATOR
	ms_message("VideoToolbox H264 codec is not supported on simulators");
#else

#if TARGET_OS_IPHONE
	if (kCFCoreFoundationVersionNumber >= kCFCoreFoundationVersionNumber_iOS_8_0) {
#else
	if (kCFCoreFoundationVersionNumber >= kCFCoreFoundationVersionNumber10_8) {
#endif
		ms_message("Registering VideoToobox H264 codec");
		ms_factory_register_filter(factory, &ms_VideoToolboxH264Encoder_desc);
		ms_factory_register_filter(factory, &ms_VideoToolboxH264Decoder_desc);
	} else {
		ms_message("Cannot register VideoToolbox H264 codec. That "
			"requires iOS 8 or MacOSX 10.8");
	}
	
#endif
}


