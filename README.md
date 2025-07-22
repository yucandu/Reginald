This thing fetches the latest Blue Jays game info from https://statsapi.mlb.com/api/v1/schedule, and then uses that to get the BIG JSON from https://statsapi.mlb.com/api/v1.1/game/######/feed/live.  

This JSON can be over 1MB total, and streaming parsing is a PITA to code, so I went with an ESP32S3F4H2 aka the S3 "Supermini" dev board that has 2MB of PSRAM.  With the 240x320 TFT sprite set to a color bit depth of 8, we have just barely enough memory to fit everything without crashing.  Maybe.  Unless somebody throws too many pitches and the JSON gets too big.  

I use my own local NTP server, 192.168.50.197, you should probably change that to pool.ntp.org or something so it actually works.

I've built this into a device with a whole bunch of buttons, a photosensor, and a speaker that I am going to develop further later, but I figured some folks might want just the MLB code, so I'm uploading that part here before further development.
