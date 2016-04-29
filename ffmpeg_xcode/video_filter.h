//
//  video_filter.h
//  ffmpeg_xcode
//
//  Created by wlanjie on 16/4/23.
//  Copyright © 2016年 com.wlanjie.ffmpeg. All rights reserved.
//

#ifndef video_filter_h
#define video_filter_h

#include <stdio.h>
#include "libavfilter/avfilter.h"
#include "libavutil/bprint.h"
#include "libavutil/pixdesc.h"
#include "open_files.h"

FilterGraph* init_filtergraph(InputStream *ist, OutputStream *ost);
int configure_filtergraph(FilterGraph *fg);

#endif /* video_filter_h */
