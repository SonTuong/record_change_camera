# Detect and record change from rtsp stream (camera)
I couldn't find any software for my need so I wrote one. If you want to use it just change the defines (I hope it is self explained) and compile with 

g++ -w cam.c -o cam $(pkg-config --cflags --libs libavformat libswscale libavcodec libavutil) -lpthread

Some TODOs:
- Omit glitch because of network
- Detect sound level also
