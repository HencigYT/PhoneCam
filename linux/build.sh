#!/bin/sh
# Builds PhoneCam-linux.jar (needs JDK 17+).
set -e
cd "$(dirname "$0")"
rm -rf out && mkdir -p out dist
javac --release 17 -encoding UTF-8 -d out $(find src -name '*.java')
printf 'Main-Class: hu.phonecam.linux.Main\n' > out/manifest.txt
jar cfm dist/PhoneCam-linux.jar out/manifest.txt -C out hu
echo "dist/PhoneCam-linux.jar"
