FFmpeg README
=============

FFmpeg is a collection of libraries and tools to process multimedia content
such as audio, video, subtitles and related metadata.

## Changes in Mad Jetson Port

I compiled this together to have a kinda "ultimate ffmpeg" for the jetson.

If you just want an easy way to use the hw decoders and encoders of the jetson, I highly recommend just using [jetson-ffmpeg](https://github.com/jocover/jetson-ffmpeg) instead. I mainly created this to have a ffmpeg that also contains the normal software encoders/decoders and have other goodies.

Currently, this version combines a updated version of the jetson-ffmpeg patches, the official nvidia hw decoder as well as fixes to make ffmpeg work with yt-dlp (see changes for more details).

This is based on upstream ffmpeg using the same branch (currently 4.4).

### Changes

 - 3df187a5e0: Added the patch from [jetson-ffmpeg](https://github.com/jocover/jetson-ffmpeg) and fixed changes so it works with this newer ffmpeg release
 - 848700a1c5: Added the official nvidia hw decoder patch as described and attached in [this email](http://ffmpeg.org/pipermail/ffmpeg-devel/2020-June/263746.html) (again fixed a bit to work here)
 - b49957d53f: Added a [suggested fix](http://ffmpeg.org/pipermail/ffmpeg-devel/2021-May/280189.html) for dts correction to make yt-dlp merge yt videos properly

### Building and/or installation

I did the installation a long time ago, so the steps may not work or need troubleshooting. I compiled it on the Jetson itself which is based on L4T 32.5.2 (Ubuntu 18.04).

I *think* I installed the following packages to satisfy ffmpegs (dev) dependencies:

```bash
sudo apt install bzip2 fontconfig libfribidi{0,-dev} gmpc{,-dev} gnutls-bin lame libass{9,-dev} libavc1394-{0,dev} libbluray{2,-dev} libdrm{2,-dev} libfreetype6{,-dev} libmodplug{1,-dev} libraw1394-{11,dev} librsvg2{-2,-dev} libsoxr{0,-dev} libtheora{0,-dev} libva{2,-dev} libva-drm2 libva-x11-2 libvdpau{1,-dev} libvorbisenc2 libvorbis{0a,-dev} libvpx{5,-dev} libwebp{6,-dev} libx11{-6,-dev} libx264-{152,dev} libx265-{146,dev} libxcb1{,-dev} libxext{6,-dev} libxml2{,-dev} libxv{1,-dev} libxvidcore{4,-dev} libopencore-amr{nb0,nb-dev,wb0,wb-dev} opus-tools libsdl2-dev speex v4l-utils zlib1g{,-dev} libopenjp2-7{,-dev} libssh-{4,dev} libspeex{1,-dev}
```

(Note the "{a,b,..}" is a bash specific expansion (bashism) and will not work in a plain posix shell)

The repo was then configured as follows:

```sh
./configure \
  --prefix=/usr/local \
  --extra-cflags="-I/usr/local/include" \
  --extra-ldflags="-L/usr/local/lib" \
  --disable-debug \
  --disable-stripping \
  --enable-lto \
  --enable-fontconfig \
  --enable-gmp \
  --enable-gnutls \
  --enable-gpl \
  --enable-ladspa \
  --enable-libass \
  --enable-libbluray \
  --enable-libdrm \
  --enable-libfreetype \
  --enable-libfribidi \
  --enable-libmodplug \
  --enable-libmp3lame \
  --enable-libopencore_amrnb \
  --enable-libopencore_amrwb \
  --enable-libopenjpeg \
  --enable-libopus \
  --enable-libpulse \
  --enable-librsvg \
  --enable-libsoxr \
  --enable-libspeex \
  --enable-libssh \
  --enable-libtheora \
  --enable-libv4l2 \
  --enable-libvorbis \
  --enable-libvpx \
  --enable-libwebp \
  --enable-libx264 \
  --enable-libx265 \
  --enable-libxcb \
  --enable-libxml2 \
  --enable-libxvid \
  --enable-version3 \
  --enable-nvmpi \
  --enable-nvv4l2dec --enable-libv4l2 --enable-shared --extra-libs="-L/usr/lib/aarch64-linux-gnu/tegra -lnvbuf_utils" --extra-cflags="-I /usr/src/jetson_multimedia_api/include/"
```

After this, running `make` should so the trick (the [release section](https://github.com/LinusCDE/mad-jetson-ffmpeg/releases) has my compiled build directory, so you may skip this step if it is too much hassle).

At long last, you should probably first uninstall the ffmpeg installed with apt and then install this with `sudo make install` (or use it standalone).

### New encoders and decoders

This version should have the vast majority of normal software encoders and decoders. Additionally these will be included:

Additional decoders:

```
 V..... h264_nvmpi           h264 (nvmpi) (codec h264)
 V..... h264_nvv4l2dec       h264 (nvv4l2dec) (codec h264)
 V..... hevc_nvmpi           hevc (nvmpi) (codec hevc)
 V..... hevc_nvv4l2dec       hevc (nvv4l2dec) (codec hevc)
 V..... mpeg2_nvmpi          mpeg2 (nvmpi) (codec mpeg2video)
 V..... mpeg2_nvv4l2dec      mpeg2 (nvv4l2dec) (codec mpeg2video)
 V..... mpeg4_nvmpi          mpeg4 (nvmpi) (codec mpeg4)
 V..... mpeg4_nvv4l2dec      mpeg4 (nvv4l2dec) (codec mpeg4)
 V..... vp8_nvmpi            vp8 (nvmpi) (codec vp8)
 V..... vp8_nvv4l2dec        vp8 (nvv4l2dec) (codec vp8)
 V..... vp9_nvmpi            vp9 (nvmpi) (codec vp9)
 V..... vp9_nvv4l2dec        vp9 (nvv4l2dec) (codec vp9)
```

Additional encoders:

```
 V..... h264_nvmpi           nvmpi H.264 encoder wrapper (codec h264)
 V..... hevc_nvmpi           nvmpi HEVC encoder wrapper (codec hevc)
```

To easily see all supported flags for the encoders use `ffmpeg -h encoder=<name_here>`.

### Word of advice

When using an hardware encoder (nvmpi being jetson-ffmpeg and nvv4l2.. being the offical ones), I recommend setting the minimum video bitrate to your target one.
I had horrible quality when not specifying it, as the hardware encoder seems to use a far too low bitrate by default unless forced not to do so.

This was one command I sometimes used for conversion from h264 to h264 (to reduce size). But using SW-Encoding still seems better and not that much smaller despite testing A LOT:

```sh
ffmpeg -fflags +igndts -vcodec h264_nvmpi -i "$input_file" -vcodec h264_nvmpi -profile:v baseline -level:v 4.1 -preset:v slow -rc vbr -minrate 256k -b:v 4000k -maxrate 10000k -bufsize 50M -g 360 $extra_flags "$output_file"
```

## Libraries

* `libavcodec` provides implementation of a wider range of codecs.
* `libavformat` implements streaming protocols, container formats and basic I/O access.
* `libavutil` includes hashers, decompressors and miscellaneous utility functions.
* `libavfilter` provides a mean to alter decoded Audio and Video through chain of filters.
* `libavdevice` provides an abstraction to access capture and playback devices.
* `libswresample` implements audio mixing and resampling routines.
* `libswscale` implements color conversion and scaling routines.

## Tools

* [ffmpeg](https://ffmpeg.org/ffmpeg.html) is a command line toolbox to
  manipulate, convert and stream multimedia content.
* [ffplay](https://ffmpeg.org/ffplay.html) is a minimalistic multimedia player.
* [ffprobe](https://ffmpeg.org/ffprobe.html) is a simple analysis tool to inspect
  multimedia content.
* Additional small tools such as `aviocat`, `ismindex` and `qt-faststart`.

## Documentation

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## Contributing

Patches should be submitted to the ffmpeg-devel mailing list using
`git format-patch` or `git send-email`. Github pull requests should be
avoided because they are not part of our review process and will be ignored.
