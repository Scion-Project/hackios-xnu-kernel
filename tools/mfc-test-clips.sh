#!/bin/bash
# The video decoder's test clips (userland/src/mfc_vt.m, mfc_player.html), all made here and silent: testsrc2 and smptebars through ffmpeg, a list of each
# one's frame CRCs (NV12, as mfc_vt computes them), HEIC stills by a Mac's sips. tools/mfc-test-clips.sh DIR; copied to a test ramdisk's share/mfc by hand.
set -e
cd "$1"
E_=(-hide_banner -loglevel error -y)
L=(-f lavfi -i)
N=(-an -movflags +faststart)
X=(-x265-params log-level=error)
ffmpeg "${E_[@]}" "${L[@]}" testsrc2=s=1920x1080:r=30 -t 10 -c:v libx264 -profile:v high -bf 3 -b:v 3M -pix_fmt yuv420p -colorspace bt709 -color_primaries bt709 -color_trc bt709 "${N[@]}" a1080.mp4
ffmpeg "${E_[@]}" "${L[@]}" testsrc2=s=1920x1080:r=30 -t 10 -c:v libx265 -tag:v hvc1 -b:v 2M -pix_fmt yuv420p -x265-params log-level=error:colorprim=bt709:transfer=bt709:colormatrix=bt709 "${N[@]}" h1080.mp4
ffmpeg "${E_[@]}" "${L[@]}" testsrc2=s=1920x1080:r=30 -t 2 -c:v libx265 -tag:v hvc1 -b:v 1M -pix_fmt yuv420p10le "${X[@]}" "${N[@]}" h10.mp4
ffmpeg "${E_[@]}" "${L[@]}" testsrc2=s=3840x2160:r=30 -t 2 -c:v libx264 -profile:v high -b:v 2M -pix_fmt yuv420p "${N[@]}" a4k.mp4
ffmpeg "${E_[@]}" "${L[@]}" testsrc2=s=3840x2160:r=30 -t 2 -c:v libx265 -tag:v hvc1 -b:v 1500k -pix_fmt yuv420p "${X[@]}" "${N[@]}" h4k.mp4
ffmpeg "${E_[@]}" "${L[@]}" smptebars=s=1920x1080:r=30 -t 2 -c:v libx264 -bf 0 -pix_fmt yuv420p -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv "${N[@]}" bars709.mp4
ffmpeg "${E_[@]}" "${L[@]}" smptebars=s=640x480:r=30 -t 2 -c:v libx264 -bf 0 -pix_fmt yuv420p -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m -color_range tv "${N[@]}" bars601.mp4
ffmpeg "${E_[@]}" "${L[@]}" smptebars=s=1280x720:r=30 -t 2 -vf format=yuvj420p -c:v libx264 -bf 0 -color_range pc -colorspace bt709 -color_primaries bt709 -color_trc bt709 "${N[@]}" barsfull.mp4
ffmpeg "${E_[@]}" "${L[@]}" testsrc2=s=1920x1080:r=30 -t 2 -vf tinterlace=interleave_top,fieldorder=tff -c:v libx264 -flags +ildct+ilme -b:v 2M -pix_fmt yuv420p "${N[@]}" ilace.mp4
ffmpeg "${E_[@]}" "${L[@]}" testsrc2=s=854x482:r=30 -t 2 -c:v libx264 -b:v 500k -pix_fmt yuv420p "${N[@]}" odd.mp4
ffmpeg "${E_[@]}" "${L[@]}" testsrc2=s=1280x720:r=30 -t 4 -an -c:v libx264 -b:v 800k -g 30 -pix_fmt yuv420p -f mpegts s720.ts
ffmpeg "${E_[@]}" "${L[@]}" testsrc2=s=1920x1080:r=30 -t 4 -an -c:v libx264 -b:v 1500k -g 30 -pix_fmt yuv420p -f mpegts s1080.ts
printf '#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXTINF:4.0,\ns720.ts\n#EXT-X-DISCONTINUITY\n#EXTINF:4.0,\ns1080.ts\n#EXT-X-DISCONTINUITY\n#EXTINF:4.0,\ns720.ts\n#EXT-X-ENDLIST\n' > rc.m3u8
python3 -I - <<'P'
import subprocess, zlib, random
for f, w, h in [('a1080.mp4', 1920, 1080), ('h1080.mp4', 1920, 1080), ('odd.mp4', 854, 482), ('bars709.mp4', 1920, 1080), ('bars601.mp4', 640, 480), ('a4k.mp4', 3840, 2160), ('h4k.mp4', 3840, 2160), ('ilace.mp4', 1920, 1080)]:
    p = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-i', f, '-f', 'rawvideo', '-pix_fmt', 'nv12', '-'], stdout=subprocess.PIPE)
    n = w * h + (h // 2) * w
    with open(f + '.crc', 'w') as out:
        while True:
            b = p.stdout.read(n)
            if len(b) < n:
                break
            out.write('%08x\n' % zlib.crc32(b))
d = bytearray(open('a1080.mp4', 'rb').read())
random.seed(9)
for at in range(800000, len(d) - 100, 200000):
    for i in range(64):
        d[at + i] = random.randrange(256)
open('corrupt.mp4', 'wb').write(d)
P
