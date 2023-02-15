#!/bin/sh
# Detect if we will be able to decode the .mp4 files in data/extractor-content/video

gst-inspect-1.0 -t decoder | grep h264 --quiet
