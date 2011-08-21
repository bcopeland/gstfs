#! /bin/sh
./gstfs -d -osrc=$1,src_ext=mp3,dst_ext=wav,pipeline="filesrc name=\"_source\" ! decodebin ! audioconvert ! wavenc ! fdsink name=\"_dest\" sync=false" $2
