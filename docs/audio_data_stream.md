# Audio Data Stream
> a document to describe the data flow in _ga_ for audio

[outer event loop (in-game loop)]
hook audio @ga-hook-coreaudio (system calls)
  => *swr_convert* @ ga-hook-coreaudio::hook_ReleaseBuffer
    swrctx:
      from pDeviceFormat (from GetMixFormat @Server)
      to rtspconf->audio_device_format/audio_device_channel_layout/audio_samplerate
  - from buffer_data # buffer 0
  - to   audio_buf # buffer 1, store converted data from swr_convert(...)
=> asource
  => asource::audio_source_buffer_fill
  => *fanout* via gClient
  => asource::audio_source_buffer_fill_one
    [out]audio_buffer_t ab as a circular-queue-like buffer
	  yet bufhead <= buftail is GUARANTEED
	  data will be shifted to [0] when necessary
  - from audio_buf # buffer 1
  - to   ab # buffer 2
    registered via asource::audio_source_client_register
	registered under gClient (scope within _asource_)
// ffmpeg
=> audio encoder @encoder-audio
  => aencoder_threadproc
    - allocate and register ab # buffer 2
	=> aencoder "event" loop
	  - a/v synchronization (using pts)
	  - read from ab # buffer 2
	  - to        samples # buffer 3, store data for only one frame&both/all channels. @aencoder_threadproc
	    <= asource::audio_source_buffer_read
	  => "encode" loop
	    => *swr_convert* @encoder-audio::aencoder_threadproc
		  - occurs when rtspconf->audio_device_format != rtsp_audio_codec_format (i.e. encoder->sample_fmt)
		  swrctx (set in encoder-audio::aencoder_init, on-the-fly audio format conversion):
		    from rtspconf->audio_device_format/audio_device_channel_layout/audio_samplerate
			to encoder->sample_fmt/channel_layout/sample_rate
			- dstplanes[0] = convbuf is also allocated.
		  - from srcbuf == samples+offset(0) # buffer 3
		  - to   convbuf # buffer 3.1
		=> *avcodec_fill_audio_frame()*: fill data 
		  - from srcbuf (i.e. convbuf if swr_convert is called _or_ samples+offset otherwise) # buffer 3.?
		    - one frame contains both/all channels.
		  - to   AVFrame: snd_in->data # bufer 4
        => *avcodec_encode_audio2()*
		  - a codec AVPacket pkt is produced with data in pkt->data (i.e. buf) # buffer 5
		  - source: snd_in (i.e. snd_in->data) # buffer 4
		  - [out] AVPacket pkt with pkt->data # buffer 5
        => encoder_send_packet() == sinkserver->send_packet() == encoder-common::encoder_pktqueue_append()
		  - pktqueue: actual pkt data
		    - circular queue
			- initialised @ga-liveserver::liveserver_main()
		  - pktlist: pointers to start address of each pkt in the queue: pktqueue
		  - from pkt->data # buffer 5
		  - to   pktqueue # buffer 6, 3MB for current configuration. Size is fixed in ga-liveserver::liverserver_main()
		=> callback function _in_ encoder-common::queue_cb
		  - registered via encoder-common::encoder_pktqueue_register_callback
		  - multiple functions may be registered for one channel and all will be notified.
// live555 server
=> signalNewAudioFrameData @ga-audiolivesource (callback function)
  - registered in ::GAAudioLiveSource::GAAudioLiveSource (constructor)
  - call liveserver_taskscheduler()->triggerEvent(
      eventTriggerId = envir().taskScheduler().createEventTrigger(deliverFrame0),
	  aLiveSource = this (i.e. GAAudioLiveSource obj.)
    )
=> result @ga-mediasubsession::GAMediaSubsession::createNewStreamSource

??

// client side
=> rtspclient
  => DummySink::afterGettingFrame(5 args)
    - convert [in]clientData (void*) into sink (DummySink*)
    => sink->afterGettingFrame(4 args)
	  - sink->fReceiveBuffer+MAXFRAMING_SIZE-audio_framing == [in]buffer # buffer 0+x
      => play_audio()
        => packet_queue_put()
	      - from play_audio::avpkt.data == [in]buffer == sink->fReceiveBuffer # buffer 0+x
	      - to   [out]q (i.e. audioq) # buffer 1+x
		    - audioq:
			  - audioq->queue (list<AVPacket>): unlimited size
			  - size is configured by option: _audio-playback-queue-limit_
			  - and a dropfactor which decides the fraction of data saved after an overflow
			  - e.g. a dropfactor of 3 => 1/3 of frames will be saved if an overflow occurs.
  => packet_queue_get()
    - from [in]q (i.e. audioq) # buffer 1+x
	- to   [out]pkt # buffer 2+x
=> rtspclient
  => audio_buffer_fill()
	<= packet_queue_get()
      - from audioq # buffer 1+x
	    - initialised in ::packet_queue_init()
		  <- ::continueAfterSETUP() <- ::setupNextSubsession() <- ::init_adecoder()
	  - to   audio_buffer_fill::avpkt # buffer 2+x
	=> ::audio_buffer_decode()
	  => *avcodec_decode_audio4()* @FFmpeg
	    - from avpkt (AVPacket) # buffer 2+x
		- to   ::aframe (aframe->data) # buffer 3+x
	  => *swr_convert* @rtspclient::audio_buffer_decode()
	    swrctx:
		  from aframe->format/channel_layout/sample_rate
		    == adecoder->sample_fmt/channel_layout/sample_rate
		  to rtspconf->audio_device_format/audio_device_channel_layout/audio_samplerate
		- from aframe->data[0] # buffer 3+x
		- to   audio_buffer_decode::convbuf # buffer 3.1+x
	  => bcopy()
	    - from srcbuf (confbuf if swr_convert occurs _or_ aframe->data[0] otherwise) # buffer 3.?+x
	    - to _audiobuf+absize_ (frame by frame) # buffer 4+x
	  - from _audiobuf+abpos_ to _stream_ # buffer 5+x
  => audio_buffer_fill_sdl (callback function @SDL_OpenAudioDevice)
=> ga-client
  => Open_Audio()
    => callback @wanted.callback _in_ SDL_OpenAudioDevice()
	  - [out]buffer is named _stream_ # buffer 5+x
[outer event loop (SDL loop)]

## Appendix A: Key Formulae
- ga-common::audio_frame_to_size()
  - $framesize = frames * channels * bitspersample / 8;$

## Appendix B: Some Argument Values (where are they set)
- rtspconf->audio_encoder_codec
  - via rtspconf_load_codec
    - codec is found via ga-avcodec::ga_avcodec_find_encoder() according to the name of the codec
  - configured by ga_conf "audio-encoder"