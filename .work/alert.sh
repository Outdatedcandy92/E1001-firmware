#!/bin/sh
wpctl set-mute @DEFAULT_AUDIO_SINK@ 0
wpctl set-volume @DEFAULT_AUDIO_SINK@ 1.0
ffplay -nodisp -autoexit -loglevel quiet "/home/rudy/Documents/Hack Club/epd/.work/snd/bong.mp3"
