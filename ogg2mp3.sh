#! /bin/sh
./gstfs -f -osrc=$1,src_ext=ogg,dst_ext=mp3,pipeline="filesrc name=\"_source\" ! oggdemux ! vorbisdec ! audioconvert ! lame bitrate=160 ! fdsink name=\"_dest\" sync=false" $2
