//
//  main.c
//  ffmpeg_xcode
//
//  Created by wlanjie on 16/4/23.
//  Copyright © 2016年 com.wlanjie.ffmpeg. All rights reserved.
//

#include <stdio.h>
#include "open_files.h"
#include "compress.h"

int main(int argc, char **args) {
    open_files("/Users/wlanjie/Desktop/sintel.mp4", "/Users/wlanjie/Desktop/compress.mp4", 260, 260);
    transcode();
    return 0;
}
