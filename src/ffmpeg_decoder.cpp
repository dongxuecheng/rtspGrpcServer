#include "ffmpeg_decoder.hpp"
#include <iostream>
#include <spdlog/spdlog.h>
#include "simple-logger.hpp"

using namespace std;

namespace FFHDDecoder
{

    static inline bool check_ffmpeg_retvalue(int e, const char *call, int iLine, const char *szFile)
    {
        if (e < 0)
        {
            char errbuf[128];
            av_strerror(e, errbuf, sizeof(errbuf));
            std::cout << "FFMPEGDecoder error " << call << ", code = " << e
                      << " (" << errbuf << ") in file " << szFile << ":" << iLine << std::endl;
            return false;
        }
        return true;
    }

#define checkFFMPEG(call) check_ffmpeg_retvalue(call, #call, __LINE__, __FILE__)

    class FFmpegDecoderImpl : public FFmpegDecoder
    {
    public:
        FFmpegDecoderImpl()
        {
            m_packet = av_packet_alloc();
            m_frame = av_frame_alloc();
        }

        ~FFmpegDecoderImpl() override
        {
            close();
        }

        bool open(AVCodecID codec_id, uint8_t *extradata, int extradata_size)
        {
            // 1. 查找解码器
            const AVCodec *codec = avcodec_find_decoder(codec_id);
            if (!codec)
            {
                INFOE("FFmpeg error: Unsupported codec ID");
                return false;
            }

            // 2. 分配解码器上下文
            m_ctx = avcodec_alloc_context3(codec);
            if (!m_ctx)
            {
                INFOE("FFmpeg error: Could not allocate video codec context");
                return false;
            }

            // 3. 复制 extradata (SPS/PPS 等头部信息)
            if (extradata && extradata_size > 0)
            {
                m_ctx->extradata = (uint8_t *)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (m_ctx->extradata)
                {
                    memcpy(m_ctx->extradata, extradata, extradata_size);
                    m_ctx->extradata_size = extradata_size;
                }
            }

            // 4. 打开解码器
            //
            // 多线程解码（关键）：AVCodecContext 的 thread_count 默认是 1，也就是单线程软解。
            // 2560x1440 HEVC 单线程大约 20~25ms/帧，再叠加 sws_scale 转 BGR 和 JPEG 编码，
            // 整条流水线就会逼近甚至超过源帧间隔（20fps → 50ms），使取流速度低于源帧率：
            // RTSP 接收缓冲持续堆积，表现为“播着播着延迟越来越高”。
            //
            // 线程数用 RTSP_DEC_THREADS 可调（默认 4）。注意不要用 0/AUTO：
            // 在 72 核机器上会按核数创建线程，切片并行的线程唤醒/同步开销反而变大，
            // 也会和同流的编码线程抢缓存，实测比 1~4 线程更慢。
            // 用 FF_THREAD_SLICE（WPP/切片并行）不引入帧重排序延迟，适合低延迟直播。
            {
                int dec_threads = 4;
                if (const char *env_threads = std::getenv("RTSP_DEC_THREADS"))
                {
                    dec_threads = std::atoi(env_threads);
                }
                if (dec_threads > 0)
                {
                    m_ctx->thread_count = dec_threads;
                    m_ctx->thread_type = FF_THREAD_SLICE;
                }
            }

            if (!checkFFMPEG(avcodec_open2(m_ctx, codec, nullptr)))
            {
                return false;
            }

            INFO("FFmpeg decoder threads: {} (thread_type={})", m_ctx->thread_count, m_ctx->thread_type);

            return true;
        }

        void close()
        {
            if (m_sws_ctx)
            {
                sws_freeContext(m_sws_ctx);
                m_sws_ctx = nullptr;
            }
            if (m_ctx)
            {
                avcodec_free_context(&m_ctx);
            }
            if (m_frame)
            {
                av_frame_free(&m_frame);
            }
            if (m_packet)
            {
                av_packet_free(&m_packet);
            }
        }

        bool send_packet(const uint8_t *pData, int nSize, int64_t pts) override
        {
            if (!m_ctx)
                return false;

            if (pData == nullptr || nSize == 0)
            {
                return checkFFMPEG(avcodec_send_packet(m_ctx, nullptr));
            }

            av_packet_unref(m_packet);
            m_packet->data = const_cast<uint8_t *>(pData);
            m_packet->size = nSize;
            m_packet->pts = pts;

            int ret = avcodec_send_packet(m_ctx, m_packet);
            if (ret < 0)
            {
                if (ret == AVERROR(EINVAL) || ret == AVERROR_INVALIDDATA)
                {
                    std::cout << "FFMPEGDecoder warning: invalid packet skipped\n";
                    return false;
                }
                if (ret == AVERROR(EAGAIN))
                {
                    // 内部缓冲区已满，需要先 drain 输出帧，当前包稍后再尝试
                    return false;
                }
                return checkFFMPEG(ret);
            }

            return true;
        }

        bool receive_frame(AVFrame **pOutFrame) override
        {
            if (!m_ctx || !m_frame)
                return false;

            // 花屏判定：先清零错误标志，避免复用的 AVFrame 残留上一帧的标志
            // （libavcodec 只在“出帧了但解码出错”时置位，靠它判定比像素域猜测准得多）
            m_frame->decode_error_flags = 0;

            // 尝试从解码器接收解码后的原始帧
            int ret = avcodec_receive_frame(m_ctx, m_frame);

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                // EAGAIN: 需要更多输入包才能解码出一帧
                // EOF: 已经到达流的末尾（Flush完毕）
                return false;
            }
            else if (ret < 0)
            {
                INFOE("Error during decoding");
                return false;
            }

            recordDecodeErrorFlags(m_frame->decode_error_flags);

            *pOutFrame = m_frame;
            return true;
        }

        // 花屏信号记录
        IVideoDecoder::DecodeHealth getDecodeHealth() const override
        {
            IVideoDecoder::DecodeHealth h;
            h.corrupted_frames = m_health_corrupted.load(std::memory_order_relaxed);
            h.missing_reference = m_health_missing_ref.load(std::memory_order_relaxed);
            h.invalid_bitstream = m_health_invalid_bs.load(std::memory_order_relaxed);
            h.last_error_wall_ms = m_health_last_ms.load(std::memory_order_relaxed);
            return h;
        }

        void resetDecodeHealth() override
        {
            m_health_corrupted.store(0, std::memory_order_relaxed);
            m_health_missing_ref.store(0, std::memory_order_relaxed);
            m_health_invalid_bs.store(0, std::memory_order_relaxed);
            m_health_last_ms.store(0, std::memory_order_relaxed);
        }

        int get_width() override { return m_ctx ? m_ctx->width : 0; }
        int get_height() override { return m_ctx ? m_ctx->height : 0; }
        AVPixelFormat get_pix_fmt() override { return m_ctx ? m_ctx->pix_fmt : AV_PIX_FMT_NONE; }

        // 辅助工具：将解码出的帧转换为指定像素格式（BGR24 / NV12 / I420 / YUYV422）
        bool convert_to_format(AVFrame *frame, uint8_t *dst_buffer, int dst_linesize,
                               AVPixelFormat dst_fmt) override
        {
            if (!frame || !dst_buffer)
                return false;

            if (!frame->data[0] || frame->width <= 0 || frame->height <= 0)
            {
                return false;
            }

            AVPixelFormat src_fmt = (AVPixelFormat)frame->format;
            switch (src_fmt)
            {
            case AV_PIX_FMT_YUVJ420P:
                src_fmt = AV_PIX_FMT_YUV420P;
                break;
            case AV_PIX_FMT_YUVJ422P:
                src_fmt = AV_PIX_FMT_YUV422P;
                break;
            case AV_PIX_FMT_YUVJ444P:
                src_fmt = AV_PIX_FMT_YUV444P;
                break;
            case AV_PIX_FMT_YUVJ440P:
                src_fmt = AV_PIX_FMT_YUV440P;
                break;
            default:
                break;
            }

            // 2. 创建或获取复用的 swscale 上下文
            //    sws_getCachedContext 会校验目标格式：目标格式变化时会自动重建上下文，
            //    因此同一路流切换 BGR/YUV 不需要额外缓存。
            m_sws_ctx = sws_getCachedContext(m_sws_ctx,
                                             frame->width, frame->height, src_fmt,
                                             frame->width, frame->height, dst_fmt,
                                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

            if (!m_sws_ctx)
                return false;

            // 3. 将 color range 设置给 swscale（可选，但这能保证色彩的绝对准确，避免发灰）
            // 如果原本是全色域，这一步能保证转换成 BGR 时色彩对比度不受损
            int *inv_table = nullptr;
            int *table = nullptr;
            int src_range, dst_range, brightness, contrast, saturation;
            if (sws_getColorspaceDetails(m_sws_ctx, &inv_table, &src_range, &table, &dst_range,
                                         &brightness, &contrast, &saturation) >= 0)
            {
                // 如果原始帧明确标记了 JPEG 范围，或者是废弃的 J 格式，强制设置为全色域 1
                src_range = (frame->color_range == AVCOL_RANGE_JPEG ||
                             frame->format == AV_PIX_FMT_YUVJ420P)
                                ? 1
                                : 0;
                sws_setColorspaceDetails(m_sws_ctx, inv_table, src_range, table, dst_range,
                                         brightness, contrast, saturation);
            }

            // 4. 执行格式转换
            //    packed 格式用调用方给的行字节数；planar 格式（NV12/I420）按紧凑布局
            //    计算各平面指针与 stride（对齐为 1，不引入任何行末 padding）。
            uint8_t *dest_data[4] = {nullptr, nullptr, nullptr, nullptr};
            int dest_linesize[4] = {0, 0, 0, 0};
            if (dst_fmt == AV_PIX_FMT_BGR24 || dst_fmt == AV_PIX_FMT_YUYV422)
            {
                const int bytes_per_pixel = (dst_fmt == AV_PIX_FMT_BGR24) ? 3 : 2;
                dest_data[0] = dst_buffer;
                dest_linesize[0] = dst_linesize > 0 ? dst_linesize : frame->width * bytes_per_pixel;
            }
            else if (av_image_fill_arrays(dest_data, dest_linesize, dst_buffer, dst_fmt,
                                          frame->width, frame->height, 1) < 0)
            {
                spdlog::error("av_image_fill_arrays failed for dst format {}", static_cast<int>(dst_fmt));
                return false;
            }

            int ret = sws_scale(m_sws_ctx, frame->data, frame->linesize, 0, frame->height,
                                dest_data, dest_linesize);
            return ret > 0;
        }

        // 便捷包装：转换为 BGR24（OpenCV Mat 默认布局）
        bool convert_to_bgr(AVFrame *frame, uint8_t *bgr_buffer, int bgr_linesize) override
        {
            return convert_to_format(frame, bgr_buffer, bgr_linesize, AV_PIX_FMT_BGR24);
        }

    private:
        // 把 AVFrame::decode_error_flags 归入花屏计数。
        // 这些标志的含义（libavutil/frame.h）：
        //   MISSING_REFERENCE   缺少参考帧 —— 必然花屏（画面出现灰块/拖影）
        //   CONCEALMENT_ACTIVE  解码器已做错误掩盖 —— 画面上就是花屏本身
        //   INVALID_BITSTREAM   码流非法/损坏
        //   DECODE_SLICES       多 slice 独立解码（不是错误，忽略）
        void recordDecodeErrorFlags(int flags)
        {
            constexpr int ERR_MASK = FF_DECODE_ERROR_MISSING_REFERENCE |
                                     FF_DECODE_ERROR_CONCEALMENT_ACTIVE |
                                     FF_DECODE_ERROR_INVALID_BITSTREAM;
            if ((flags & ERR_MASK) == 0)
                return;

            m_health_corrupted.fetch_add(1, std::memory_order_relaxed);
            if (flags & FF_DECODE_ERROR_MISSING_REFERENCE)
                m_health_missing_ref.fetch_add(1, std::memory_order_relaxed);
            if (flags & FF_DECODE_ERROR_INVALID_BITSTREAM)
                m_health_invalid_bs.fetch_add(1, std::memory_order_relaxed);
            m_health_last_ms.store(static_cast<int64_t>(
                                       std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count()),
                                   std::memory_order_relaxed);
        }

        AVCodecContext *m_ctx = nullptr;
        AVPacket *m_packet = nullptr;
        AVFrame *m_frame = nullptr;
        SwsContext *m_sws_ctx = nullptr;

        // 花屏统计（原子：IO 线程写，gRPC 线程读）
        std::atomic<uint64_t> m_health_corrupted{0};
        std::atomic<uint64_t> m_health_missing_ref{0};
        std::atomic<uint64_t> m_health_invalid_bs{0};
        std::atomic<int64_t> m_health_last_ms{0};
    };

    std::shared_ptr<FFmpegDecoder> create_ffmpeg_decoder(AVCodecID codec_id, uint8_t *extradata, int extradata_size)
    {
        std::shared_ptr<FFmpegDecoderImpl> instance(new FFmpegDecoderImpl());
        if (!instance->open(codec_id, extradata, extradata_size))
        {
            instance.reset();
        }
        return instance;
    }
}