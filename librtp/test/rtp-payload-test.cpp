#include "rtp-payload.h"
#include "rtp-profile.h"
#include "rtcp-header.h"
#include "mpeg-ps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
#define strcasecmp _stricmp
#endif

struct rtp_payload_test_t
{
	int payload;
	const char* encoding;

	FILE* frtp;
	FILE* frtp2;
	FILE* fout2;

	void* encoder;
	void* decoder;
	ps_demuxer_t* ps;

	size_t size;
	uint8_t packet[64 * 1024];
};

static void* rtp_alloc(void* /*param*/, int bytes)
{
	static uint8_t buffer[2 * 1024 * 1024 + 4] = { 0, 0, 0, 1, };
	assert(bytes <= sizeof(buffer) - 4);
	return buffer + 4;
}

static void rtp_free(void* /*param*/, void * /*packet*/)
{
}

static int rtp_encode_packet(void* param, const void *packet, int bytes, uint32_t /*timestamp*/, int /*flags*/)
{
	struct rtp_payload_test_t* ctx = (struct rtp_payload_test_t*)param;
	uint8_t size[2];
	size[0] = (uint8_t)((uint32_t)bytes >> 8);
	size[1] = (uint8_t)(uint32_t)bytes;
	fwrite(size, 1, sizeof(size), ctx->frtp2);
	fwrite(packet, 1, bytes, ctx->frtp2);
	return 0;
}

static int mpeg_ps_demuxer_onpacket(void* param, int stream, int codecid, int flags, int64_t pts, int64_t dts, const void* data, size_t bytes)
{
	printf("[%d] codec: %d, pts: %" PRId64 ", dts: %" PRId64 ", bytes: %d, flags: 0x%x\n", stream, codecid, pts/90, dts/90, (int)bytes, flags);
	return 0;
}

static int rtp_decode_packet(void* param, const void *packet, int bytes, uint32_t timestamp, int flags)
{
	static const uint8_t start_code[4] = { 0, 0, 0, 1 };
	struct rtp_payload_test_t* ctx = (struct rtp_payload_test_t*)param;

	static uint8_t buffer[2 * 1024 * 1024];
	assert(bytes + 4 < sizeof(buffer));
	assert(0 == flags);

	size_t size = 0;
	if (0 == strcmp("H264", ctx->encoding) || 0 == strcmp("H265", ctx->encoding))
	{
		memcpy(buffer, start_code, sizeof(start_code));
		size += sizeof(start_code);
	}
	else if (0 == strcasecmp("mpeg4-generic", ctx->encoding))
	{
		int len = bytes + 7;
		uint8_t profile = 2;
		uint8_t sampling_frequency_index = 4;
		uint8_t channel_configuration = 2;
		buffer[0] = 0xFF; /* 12-syncword */
		buffer[1] = 0xF0 /* 12-syncword */ | (0 << 3)/*1-ID*/ | (0x00 << 2) /*2-layer*/ | 0x01 /*1-protection_absent*/;
		buffer[2] = ((profile - 1) << 6) | ((sampling_frequency_index & 0x0F) << 2) | ((channel_configuration >> 2) & 0x01);
		buffer[3] = ((channel_configuration & 0x03) << 6) | ((len >> 11) & 0x03); /*0-original_copy*/ /*0-home*/ /*0-copyright_identification_bit*/ /*0-copyright_identification_start*/
		buffer[4] = (uint8_t)(len >> 3);
		buffer[5] = ((len & 0x07) << 5) | 0x1F;
		buffer[6] = 0xFC | ((len / 1024) & 0x03);
		size = 7;
	}
	else if (0 == strcmp("AV1", ctx->encoding) || 0 == strcmp("AV1X", ctx->encoding))
	{
		static const uint8_t av1_temporal_delimiter[] = { 0x12, 0x00 };
		memcpy(buffer, av1_temporal_delimiter, sizeof(av1_temporal_delimiter));
		size += sizeof(av1_temporal_delimiter);
	}
	else if (0 == strcmp("PS", ctx->encoding))
	{
		if(!ctx->ps)
			ctx->ps = ps_demuxer_create(mpeg_ps_demuxer_onpacket, ctx);
		ps_demuxer_input(ctx->ps, (const uint8_t*)packet, bytes);
	}

	memcpy(buffer + size, packet, bytes);
	size += bytes;

	// TODO:
	// check media file
	fwrite(buffer, 1, size, ctx->fout2);

	return rtp_payload_encode_input(ctx->encoder, buffer, size, timestamp);
}

static void rtp_payload_test(int payload, const char* encoding, uint16_t seq, uint32_t ssrc, const char* rtpfile, const char* deprecated)
{
	struct rtp_payload_test_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.payload = payload;
	ctx.encoding = encoding;

	ctx.frtp = fopen(rtpfile, "rb");
	ctx.frtp2 = fopen("out.rtp", "wb");
	ctx.fout2 = fopen("out.media", "wb");

	rtp_packet_setsize(1456); // 1456(live555)

	struct rtp_payload_t handler2;
	handler2.alloc = rtp_alloc;
	handler2.free = rtp_free;
	handler2.packet = rtp_encode_packet;
	ctx.encoder = rtp_payload_encode_create(payload, encoding, seq, ssrc, &handler2, &ctx);

	struct rtp_payload_t handler1;
	handler1.alloc = rtp_alloc;
	handler1.free = rtp_free;
	handler1.packet = rtp_decode_packet;
	ctx.decoder = rtp_payload_decode_create(payload, encoding, &handler1, &ctx);

	for(int i = 0; ;i++)
	{
		uint8_t s2[2];
		if (2 != fread(s2, 1, 2, ctx.frtp))
			break;

		ctx.size = (s2[0] << 8) | s2[1];
		assert(ctx.size < sizeof(ctx.packet));
		if (ctx.size != (int)fread(ctx.packet, 1, ctx.size, ctx.frtp))
			break;

		if (ctx.packet[1] < RTCP_FIR || ctx.packet[1] > RTCP_LIMIT)
			rtp_payload_decode_input(ctx.decoder, ctx.packet, ctx.size);
	}

	fclose(ctx.frtp);
	fclose(ctx.frtp2);
	fclose(ctx.fout2);
	rtp_payload_decode_destroy(ctx.decoder);
	rtp_payload_encode_destroy(ctx.encoder);

	// todo: binary_diff(rtpfile�� deprecated)
}

// http://www.live555.com/liveMedia/public/
void rtp_payload_test(const char* file, int payload, const char* encoding)
{
	//rtp_payload_test(33, "MP2T", 24470, 1726408532, "E:\\video\\rtp\\bipbop-gear1-all.ts.rtp");
	//binary_diff("E:\\video\\rtp\\bipbop-gear1-all.ts.rtp", "out.rtp");
	//binary_diff("E:\\video\\rtp\\bipbop-gear1-all.ts", "out.media");

	//rtp_payload_test(96, "H264", 12686, 1957754144, "E:\\video\\rtp\\live555-test.h264.rtp", "E:\\video\\rtp\\live555-test.h264");
	//binary_diff("E:\\video\\rtp\\live555-test.h264.rtp", "out.rtp");
	//binary_diff("E:\\video\\rtp\\live555-test.h264", "out.media");

	//rtp_payload_test(96, "H265", 64791, 850623724, "E:\\video\\rtp\\live555-surfing.h265.rtp", "E:\\video\\rtp\\live555-surfing.h265");
	//binary_diff("E:\\video\\rtp\\live555-surfing.h265.rtp", "out.rtp");
	//binary_diff("E:\\video\\rtp\\live555-surfing.h265", "out.media");

	//rtp_payload_test(RTP_PAYLOAD_MPA, "", 51375, 62185158, "E:\\video\\rtp\\24kbps.mp3.rtp", "E:\\video\\rtp\\24kbps.mp3");
	//binary_diff("E:\\video\\rtp\\24kbps.mp3.rtp", "out.rtp");
	//binary_diff("E:\\video\\rtp\\24kbps.mp3", "out.media");

	//rtp_payload_test(RTP_PAYLOAD_MPA, "", 59489, 4023046964, "E:\\video\\rtp\\128kbps.mp3.rtp", "E:\\video\\rtp\\128kbps.mp3");
	//binary_diff("E:\\video\\rtp\\128kbps.mp3.rtp", "out.rtp");
	//binary_diff("E:\\video\\rtp\\live555-surfing.h265", "out.media");

	//rtp_payload_test(RTP_PAYLOAD_MPV, "", 6879, 2417761871, "E:\\video\\rtp\\movie2.mpv.rtp", "E:\\video\\rtp\\movie2.mpv");
	//binary_diff("E:\\video\\rtp\\movie2.mpv.rtp", "out.rtp");
	//binary_diff("E:\\video\\rtp\\movie2.mpv", "out.media");

	//rtp_payload_test(96, "MP4V-ES", 28210, 11391760, "E:\\video\\rtp\\live555-petrov.m4e.MP4V-ES.rtp", "E:\\video\\rtp\\movie2.mpv");
	//binary_diff("E:\\video\\rtp\\live555-petrov.m4e.MP4V-ES.rtp", "out.rtp");
	//binary_diff("E:\\video\\rtp\\live555-petrov.m4e", "out.media");

	//rtp_payload_test(96, "mpeg4-generic", 13353, 1082077255, "E:\\video\\rtp\\live555-test.aac.rtp", "E:\\video\\rtp\\live555-test.aac");
	//binary_diff("E:\\video\\rtp\\live555-test.aac.rtp", "out.rtp");
	//binary_diff("E:\\video\\rtp\\live555-test.aac", "out.media");

	//rtp_payload_test(96, "MP4A-LATM", 3647, 345042127, "E:\\video\\rtp\\720p.96.MP4A-LATM.rtp", "E:\\video\\rtp\\720p.96.MP4A-LATM");
	//binary_diff("E:\\video\\rtp\\720p.96.MP4A-LATM.rtp", "out.rtp");
	//binary_diff("E:\\video\\rtp\\720p.96.MP4A-LATM", "out.media");

	//rtp_payload_test(96, "VP8", 2948, 1447139175, "E:\\work\\media-server\\rtmp2hls\\192.168.31.132.6974.96.VP8.rtp", "E:\\work\\media-server\\rtmp2hls\\192.168.31.132.6974.96.VP8");
	//binary_diff("E:\\work\\media-server\\rtmp2hls\\192.168.31.132.6974.96.VP8.rtp", "out.rtp");
	//binary_diff("E:\\work\\media-server\\rtmp2hls\\192.168.31.132.6974.96.VP8", "out.media");

	//rtp_payload_test(96, "AV1", 12686, 1957754144, "C:\\Users\\Administrator\\Downloads\\1688.rtp", "1.h264");
	//rtp_payload_test(96, "PS", 12686, 1957754144, "C:\\Users\\Administrator\\Downloads\\1.rtp", "1.ps");
	rtp_payload_test(payload, encoding, 0, 0, file, "");
}
