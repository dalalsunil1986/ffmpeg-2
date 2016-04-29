//
//  filter.h
//  ffmpeg_xcode
//
//  Created by wlanjie on 16/4/20.
//  Copyright © 2016年 com.wlanjie.ffmpeg. All rights reserved.
//

#ifndef filter_h
#define filter_h

#include <stdio.h>
#include "ffmpeg.h"

struct FilterGraph *init_simple_filtergraph(InputStream *ist, OutputStream *ost);
int configure_filtergraph(FilterGraph *fg);
#endif /* filter_h */
