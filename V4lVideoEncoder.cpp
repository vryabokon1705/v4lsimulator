#include <gz/common/av/Util.hh>
#include "gz/common/ffmpeg_inc.hh"
#include "gz/common/Console.hh"
#include "gz/common/VideoEncoder.hh"
#include "gz/common/StringUtils.hh"

#include "V4lVideoEncoder.h"

using namespace gz;
using namespace common;
using namespace std;

using OutputFormat = AVOutputFormat*;

class GZ_COMMON_AV_HIDDEN V4lVideoEncoder::Implementation
{
  /// \brief Name of the file which stores the video while it is being
  ///        recorded.
  public: std::string filename;

  /// \brief libav audio video stream
  public: AVStream *videoStream = nullptr;

  /// \brief libav codec  context
  public: AVCodecContext *codecCtx = nullptr;

  /// \brief libav format I/O context
  public: AVFormatContext *formatCtx = nullptr;

  /// \brief libav output video frame (aligned to 32 bytes)
  public: AVFrame *avOutFrame = nullptr;

  /// \brief libav input image data (aligned to 32 bytes)
  public: AVFrame *avInFrame = nullptr;

  /// \brief Pixel format of the input frame. So far it is hardcoded.
  public: AVPixelFormat inPixFormat = AV_PIX_FMT_RGB24;

  /// \brief Software scaling context
  public: SwsContext *swsCtx = nullptr;

  /// \brief Line sizes of an unaligned input frame
  public: int inputLineSizes[4];

  /// \brief True if the encoder is running
  public: bool encoding = false;

  /// \brief Video encoding bit rate
  public: unsigned int bitRate = VIDEO_ENCODER_BITRATE_DEFAULT;

  /// \brief Input frame width
  public: unsigned int inWidth = 0;

  /// \brief Input frame height
  public: unsigned int inHeight = 0;

  /// \brief Encoding format
  public: std::string format = VIDEO_ENCODER_FORMAT_DEFAULT;

  /// \brief Target framerate.
  public: unsigned int fps = VIDEO_ENCODER_FPS_DEFAULT;

  /// \brief Previous time when the frame is added.
  public: std::chrono::steady_clock::time_point timePrev;

  /// \brief Time when the first frame is added.
  public: std::chrono::steady_clock::time_point timeStart;

  /// \brief Number of frames in the video
  public: uint64_t frameCount = 0;

  /// \brief Mutex for thread safety.
  public: std::mutex mutex;

  int ProcessPacket(AVPacket* avPacket);
};

int V4lVideoEncoder::Implementation::ProcessPacket(AVPacket* avPacket)
{
  avPacket->stream_index = this->videoStream->index;

  // Scale timestamp appropriately.
  if (avPacket->pts != static_cast<int64_t>(AV_NOPTS_VALUE))
  {
    avPacket->pts = av_rescale_q(
      avPacket->pts,
      this->codecCtx->time_base,
      this->videoStream->time_base);
  }

  if (avPacket->dts != static_cast<int64_t>(AV_NOPTS_VALUE))
  {
    avPacket->dts = av_rescale_q(
      avPacket->dts,
      this->codecCtx->time_base,
      this->videoStream->time_base);
  }

  // Write frame to disk
  int ret = av_interleaved_write_frame(this->formatCtx, avPacket);

  if (ret < 0)
    gzerr << "Error writing frame: " << av_err2str_cpp(ret) << std::endl;

  return ret;
}



V4lVideoEncoder::V4lVideoEncoder()
    : dataPtr(utils::MakeUniqueImpl<Implementation>())
{
  // Make sure libav is loaded.
  common::load();
}

V4lVideoEncoder::~V4lVideoEncoder()
{

}

void V4lVideoEncoder::reset()
{
  // Make sure the video has been stopped.
  this->stop();

    // set default values
  this->dataPtr->frameCount = 0;
  this->dataPtr->inWidth = 0;
  this->dataPtr->inHeight = 0;
  this->dataPtr->timePrev = {};
  this->dataPtr->bitRate = VIDEO_ENCODER_BITRATE_DEFAULT;
  this->dataPtr->fps = VIDEO_ENCODER_FPS_DEFAULT;
  this->dataPtr->format = VIDEO_ENCODER_FORMAT_DEFAULT;
  this->dataPtr->timePrev = std::chrono::steady_clock::time_point();
  this->dataPtr->timeStart = std::chrono::steady_clock::time_point();
  this->dataPtr->filename.clear();

}

bool V4lVideoEncoder::isEncoding() const {
    return this->dataPtr->encoding;
}

bool V4lVideoEncoder::start(const std::string &format,
                         const std::string &filename,
                         const unsigned int width,
                         const unsigned int height,
                         const unsigned int fps,
                         const unsigned int bitRate) {
    cout << "Starting format="
        << format
        << " filename=" << filename
        << " width=" << width
        << " height=" << height
        << " fps=" << fps
        << " bitRate=" << bitRate
        << endl;

    this->dataPtr->format =format;
    this->dataPtr->fps = fps;
    this->dataPtr->frameCount = 0;
    this->dataPtr->filename = filename;
    this->dataPtr->bitRate = bitRate;

    if (this->dataPtr->formatCtx)
    {
        avformat_free_context(this->dataPtr->formatCtx);
    }
    this->dataPtr->formatCtx = nullptr;
    OutputFormat outputFormat = nullptr;
    do {
      outputFormat = av_output_video_device_next(outputFormat);

      if (outputFormat)
      {

        cout << "Output format=" << outputFormat->name << endl;

        // Break when the output device name matches 'v4l2'
        if ( V4L_FORMAT.compare(outputFormat->name) == 0)
        {
          // Allocate the context using the correct outputFormat
          auto result = avformat_alloc_output_context2(
              &this->dataPtr->formatCtx,
              outputFormat, nullptr, this->dataPtr->filename.c_str());
          if (result < 0)
          {
            gzerr << "Failed to allocate AV context ["
                  << av_err2str_cpp(result)
                  << "]" << std::endl;
          }
          break;
        }
      }
    } while (outputFormat);

    if (!this->dataPtr->formatCtx)
    {
      gzerr << "Unable to allocate format context. Video encoding not started\n";
      this->reset();
      return false;
    }

    const auto codecId = this->dataPtr->formatCtx->oformat->video_codec;

    cout << "Code id: " << codecId << ", codec name: " << avcodec_get_name(codecId) << endl;

    auto* encoder = avcodec_find_encoder(codecId);

    if (!encoder)
    {
      gzerr << "Codec for["
            << avcodec_get_name(codecId)
            << "] not found. Video encoding is not started.\n";
      this->reset();
      return false;
    }

    gzmsg << "Using encoder " << encoder->name << std::endl;

      // Create a new video stream
    this->dataPtr->videoStream = avformat_new_stream(this->dataPtr->formatCtx, nullptr);

    if (!this->dataPtr->videoStream)
    {
      gzerr << "Could not allocate stream. Video encoding is not started\n";
      this->reset();
      return false;
    }
    this->dataPtr->videoStream->id = this->dataPtr->formatCtx->nb_streams-1;

      // Allocate a new video context
    this->dataPtr->codecCtx = avcodec_alloc_context3(encoder);

    if (!this->dataPtr->codecCtx)
    {
      gzerr << "Could not allocate an encoding context."
            << "Video encoding is not started\n";
      this->reset();
      return false;
    }

      // some formats want stream headers to be separate
  if (this->dataPtr->formatCtx->oformat->flags & AVFMT_GLOBALHEADER)
  {
    this->dataPtr->codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  // Frames per second
  this->dataPtr->codecCtx->time_base.den = this->dataPtr->fps;
  this->dataPtr->codecCtx->time_base.num = 1;

  // The video stream must have the same time base as the context
  this->dataPtr->videoStream->time_base.den = this->dataPtr->fps;
  this->dataPtr->videoStream->time_base.num = 1;

  // Bitrate
  this->dataPtr->codecCtx->bit_rate = this->dataPtr->bitRate;

  // The resolution must be divisible by two
  this->dataPtr->codecCtx->width = width % 2 == 0 ? width : width + 1;
  this->dataPtr->codecCtx->height = height % 2 == 0 ? height : height + 1;

  // Emit one intra-frame every 10 frames
  this->dataPtr->codecCtx->gop_size = 10;
  this->dataPtr->codecCtx->max_b_frames = 1;
  this->dataPtr->codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  this->dataPtr->codecCtx->thread_count = 5;


  // Set the codec id
  this->dataPtr->codecCtx->codec_id = codecId;


  if (this->dataPtr->codecCtx->codec_id == AV_CODEC_ID_MPEG1VIDEO)
  {
    // Needed to avoid using macroblocks in which some coeffs overflow.
    // This does not happen with normal video, it just happens here as
    // the motion of the chroma plane does not match the luma plane.
    this->dataPtr->codecCtx->mb_decision = 2;
  }

  if (this->dataPtr->codecCtx->codec_id == AV_CODEC_ID_H264)
  {
    av_opt_set(this->dataPtr->codecCtx->priv_data, "preset", "slow", 0);
    av_opt_set(this->dataPtr->videoStream->priv_data, "preset", "slow", 0);
  }

  this->dataPtr->codecCtx->sw_pix_fmt = this->dataPtr->codecCtx->pix_fmt;

    int ret = avcodec_open2(this->dataPtr->codecCtx, encoder, 0);
  if (ret < 0)
  {
    gzerr << "Could not open video codec: " << av_err2str_cpp(ret)
          << ". Video encoding is not started\n";

    this->reset();
    return false;
  }

  this->dataPtr->avOutFrame = av_frame_alloc();

  if (!this->dataPtr->avOutFrame)
  {
    gzerr << "Could not allocate video frame. Video encoding is not started\n";
    this->reset();
    return false;
  }

    // we misuse sw_pix_fmt a bit, as docs say it is unused in encoders
  this->dataPtr->avOutFrame->format = this->dataPtr->codecCtx->sw_pix_fmt;
  this->dataPtr->avOutFrame->width = this->dataPtr->codecCtx->width;
  this->dataPtr->avOutFrame->height = this->dataPtr->codecCtx->height;

    // av_image_alloc() could also allocate the image, but av_frame_get_buffer()
  // allocates a refcounted buffer, which is easier to manage
  if (av_frame_get_buffer(this->dataPtr->avOutFrame, 32) > 0)
  {
    gzerr << "Could not allocate raw picture buffer. "
           << "Video encoding is not started\n";
    this->reset();
    return false;
  }

    // Copy parameters from the context to the video stream
  // codecpar was implemented in ffmpeg version 3.1
  ret = avcodec_parameters_from_context(
      this->dataPtr->videoStream->codecpar, this->dataPtr->codecCtx);
  if (ret < 0)
  {
    gzerr << "Could not copy the stream parameters:" << av_err2str_cpp(ret)
          << ". Video encoding not started\n";
    return false;
  }

   double muxMaxDelay = 0.7f;
  this->dataPtr->formatCtx->max_delay =
    static_cast<int>(muxMaxDelay * AV_TIME_BASE);

  // Open the video stream
  if (!(this->dataPtr->formatCtx->oformat->flags & AVFMT_NOFILE))
  {
    ret = avio_open(&this->dataPtr->formatCtx->pb,
        this->dataPtr->filename.c_str(), AVIO_FLAG_WRITE);

    if (ret < 0)
    {
      gzerr << "Could not open '" << this->dataPtr->filename << "'. "
            << av_err2str_cpp(ret) << ". Video encoding is not started\n";
      this->reset();
      return false;
    }
  }

    // Write the stream header, if any.
  ret = avformat_write_header(this->dataPtr->formatCtx, nullptr);
  if (ret < 0)
  {
    gzerr << "Error occured when opening output file: " << av_err2str_cpp(ret)
          << ". Video encoding is not started\n";
    this->reset();
    return false;
  }

  this->dataPtr->encoding = true;
  return true;
}

bool V4lVideoEncoder::stop() {
    cout << "Stopping" << endl;
    this->dataPtr->encoding = false;
    return true;
}

bool V4lVideoEncoder::addFrame(const unsigned char *frame,
    const unsigned int width,
    const unsigned int height,
    const std::chrono::steady_clock::time_point &timestamp) {

  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);

  if (!this->dataPtr->encoding)
  {
    gzerr << "Start encoding before adding a frame\n";
    return false;
  }

  auto dt = timestamp - this->dataPtr->timePrev;

  // Skip frames that arrive faster than the video's fps
  double period = 1.0/this->dataPtr->fps;
  if (this->dataPtr->frameCount > 0u &&
      dt < std::chrono::duration<double>(period))
    return false;

  if (this->dataPtr->frameCount == 0u)
    this->dataPtr->timeStart = timestamp;

  this->dataPtr->timePrev = timestamp;

  // Cause the sws to be recreated on image resize
  if (this->dataPtr->swsCtx &&
      (this->dataPtr->inWidth != width || this->dataPtr->inHeight != height))
  {
    sws_freeContext(this->dataPtr->swsCtx);
    this->dataPtr->swsCtx = nullptr;

    if (this->dataPtr->avInFrame)
      av_frame_free(&this->dataPtr->avInFrame);
    this->dataPtr->avInFrame = nullptr;
  }

  if (!this->dataPtr->swsCtx)
  {
    this->dataPtr->inWidth = width;
    this->dataPtr->inHeight = height;

    if (!this->dataPtr->avInFrame)
    {
      this->dataPtr->avInFrame = av_frame_alloc();
      this->dataPtr->avInFrame->width = this->dataPtr->inWidth;
      this->dataPtr->avInFrame->height = this->dataPtr->inHeight;
      this->dataPtr->avInFrame->format = this->dataPtr->inPixFormat;

      av_frame_get_buffer(this->dataPtr->avInFrame, 32);
    }

    av_image_fill_linesizes(this->dataPtr->inputLineSizes,
                            this->dataPtr->inPixFormat,
                            this->dataPtr->inWidth);

    this->dataPtr->swsCtx = sws_getContext(
        this->dataPtr->inWidth,
        this->dataPtr->inHeight,
        this->dataPtr->inPixFormat,
        this->dataPtr->codecCtx->width,
        this->dataPtr->codecCtx->height,
        // we misuse this field a bit, as docs say it is unused in encoders
        this->dataPtr->codecCtx->sw_pix_fmt,
        0, nullptr, nullptr, nullptr);

    if (this->dataPtr->swsCtx == nullptr)
    {
      gzerr << "Error while calling sws_getContext\n";
      return false;
    }
  }

  // encode

  // copy the unaligned input buffer to the 32-byte-aligned avInFrame
  av_image_copy(
      this->dataPtr->avInFrame->data, this->dataPtr->avInFrame->linesize,
      &frame, this->dataPtr->inputLineSizes,
      this->dataPtr->inPixFormat,
      this->dataPtr->inWidth, this->dataPtr->inHeight);

  sws_scale(this->dataPtr->swsCtx,
      this->dataPtr->avInFrame->data,
      this->dataPtr->avInFrame->linesize,
      0, this->dataPtr->inHeight,
      this->dataPtr->avOutFrame->data,
      this->dataPtr->avOutFrame->linesize);

  auto* frameToEncode = this->dataPtr->avOutFrame;

  // compute frame number based on timestamp of current image
  auto timeSinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
      timestamp - this->dataPtr->timeStart);
  double durationSec = timeSinceStart.count() / 1000.0;
  uint64_t frameNumber = static_cast<uint64_t>(durationSec / period);

  uint64_t frameDiff = frameNumber + 1 - this->dataPtr->frameCount;

  int ret = 0;

  // make sure we have continuous pts (frame number) otherwise some decoders
  // may not be happy. So encode more (duplicate) frames until the current frame
  // number
  for (uint64_t i = 0u;
       i < frameDiff && (ret >= 0 || ret == AVERROR(EAGAIN));
       ++i)
  {
    frameToEncode->pts = this->dataPtr->frameCount++;

    AVPacket* avPacket = av_packet_alloc();

    avPacket->data = nullptr;
    avPacket->size = 0;

    ret = avcodec_send_frame(this->dataPtr->codecCtx,
                                 frameToEncode);

    // This loop will retrieve and write available packets
    while (ret >= 0)
    {
      ret = avcodec_receive_packet(this->dataPtr->codecCtx, avPacket);

      // Potential performance improvement: Queue the packets and write in
      // a separate thread.
      if (ret >= 0)
        ret = this->dataPtr->ProcessPacket(avPacket);
    }

    av_packet_unref(avPacket);
  }
  return ret >= 0 || ret == AVERROR(EAGAIN);
}