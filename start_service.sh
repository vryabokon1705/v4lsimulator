gz service -s /camera/record_video1 --reqtype gz.msgs.VideoRecord --reptype gz.msgs.Boolean --timeout 300 --req 'start: true, format:"v4l2", save_filename:"/dev/video2"'
