gauge_mod - your own PNG gauges on the dashboard (NFSU2HeadTrack)
===================================================================

The in-car rev counter and speedometer use the PNG files named in gauges.ini.
If a file is missing or the entry is empty, the built-in drawn gauge is used instead.

Files in this folder
  tachometer.png   rev counter face (0-9 x1000 rpm, red from 7500)
  speedometer.png  speedometer face (0-320 km/h)
  needle.png       needle used by both gauges
  gauges.ini       which files to use, needle sweep and scale, digital readout

Making your own
1. Face: a square PNG (512x512 or 1024x1024 works well) with the round gauge filling the image
   edge to edge and transparent corners. Draw the scale, numbers, red zone and logos into it.
2. Needle: a square PNG with the same size, transparent background, the needle pointing straight
   up to 12 o'clock and its pivot exactly at the image center.
3. In gauges.ini set MinAngle / MaxAngle to where your scale starts and ends (degrees from
   12 o'clock, clockwise) and MaxValue to the value at the end of the scale.
4. Restart the game. NFSU2HeadTrack.log in the scripts folder lists the files it loaded.

You can keep several designs here and switch between them by changing the file names in
gauges.ini, e.g. Face=tachometer_carbon.png.

Gauge position and size in the car are set in scripts\NFSU2HeadTrack.ini, section [GAUGES].
