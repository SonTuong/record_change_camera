#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <termios.h>

bool kbhit()
{
    termios term;
    tcgetattr(0, &term);

    termios term2 = term;
    term2.c_lflag &= ~ICANON;
    tcsetattr(0, TCSANOW, &term2);

    int byteswaiting;
    ioctl(0, FIONREAD, &byteswaiting);

    tcsetattr(0, TCSANOW, &term);

    return byteswaiting > 0;
}

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
}

#define BLACK_W 500
#define BLACK_H 44
#define Y_CHANGE_LEVEL 20
#define CONTINUOUS_DIFF 3
#define NUM_DIFF_PIX 2000
#define RECORD_EARLY 4000
#define RECORD_TIME 7000

struct avpacket_node
{
    struct avpacket_node* next;
    AVPacket packet;
};

    unsigned int num_packet = 0;
    struct avpacket_node* first_node;
    struct avpacket_node* last_node;
    unsigned int record_pts = 0;
    volatile unsigned int write_to_time = 0;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond1 = PTHREAD_COND_INITIALIZER; 
    AVFormatContext* oc;
void* write_file(void* p)
{
    unsigned int c = 0;
    unsigned int last_packet_pts = 0;
    while(true)
    {
        pthread_mutex_lock(&lock);
        if (num_packet < 2)
        {
            pthread_cond_wait(&cond1, &lock);
        }
        if (first_node->packet.pts < write_to_time)
        {
            if (!c)
            {
                last_packet_pts = first_node->packet.pts;
            }
            else
            {
                record_pts += first_node->packet.pts - last_packet_pts;
                last_packet_pts = first_node->packet.pts;
            }
            first_node->packet.dts = first_node->packet.pts = record_pts;
            av_write_frame(oc,&first_node->packet);
            av_free_packet(&first_node->packet);
            struct avpacket_node *temp = (avpacket_node*)first_node->next;
            free(first_node);
            first_node = temp;
            num_packet--;
            c++;
        }
        else
        {
            write_to_time = 0;
            printf("written %d packets\n", c);
            pthread_mutex_unlock(&lock);
            record_pts += 50;
            break;
        }
        pthread_mutex_unlock(&lock);
    }
    
}
int main(int argc, char** argv) {

    // Open the initial context variables that are needed
    SwsContext *img_convert_ctx;
    AVFormatContext* format_ctx = avformat_alloc_context();
    AVCodecContext* codec_ctx = NULL;
    
    int video_stream_index;
    struct timespec t1, t2;
    // Register everything
    av_register_all();
    avformat_network_init();
    
    first_node = (avpacket_node*) malloc(sizeof(struct avpacket_node));
    av_init_packet(&first_node->packet);
    last_node = first_node;
    
    int record_early;
    int record_time;

    //open RTSP
    AVDictionary *options = NULL;
    format_ctx->max_delay = 0;
    printf("dict_set %d\n",av_dict_set(&options, "reorder_queue_size", "-1", 0));
    if (avformat_open_input(&format_ctx, "rtsp://admin:password@192.168.1.142/onvif1",
            NULL, NULL) != 0) {
        return EXIT_FAILURE;
    }
    av_dict_free(&options);
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        return EXIT_FAILURE;
    }

    //search video stream
    for (int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
            video_stream_index = i;
    }

    AVPacket *packet = &first_node->packet;

    //open output file
    AVFormatContext* output_ctx = avformat_alloc_context();

    AVStream* stream = NULL;
    int cnt = 0;

    //start reading packets from stream and write them to file
    av_read_play(format_ctx);    //play RTSP

    // Get the codec
    AVCodec *codec = NULL;
    printf("%d\n",format_ctx->streams[video_stream_index]->codec->codec_id);
    codec = avcodec_find_decoder(format_ctx->streams[video_stream_index]->codec->codec_id);
    if (!codec) {
        exit(1);
    }

    AVOutputFormat* fmt = av_guess_format(NULL,"test.mkv",NULL);
    oc = avformat_alloc_context();
    oc->oformat = fmt;
    avio_open2(&oc->pb, "test.mkv", AVIO_FLAG_WRITE,NULL,NULL);
    
    // Add this to allocate the context by codec
    codec_ctx = avcodec_alloc_context3(codec);

    avcodec_get_context_defaults3(codec_ctx, codec);
    avcodec_copy_context(codec_ctx, format_ctx->streams[video_stream_index]->codec);

    if (avcodec_open2(codec_ctx, codec, NULL) < 0)
        exit(1);
    std::ofstream output_file;
    img_convert_ctx = sws_getContext(codec_ctx->width, codec_ctx->height,
            codec_ctx->pix_fmt, codec_ctx->width, codec_ctx->height, AV_PIX_FMT_RGB24,
            SWS_BICUBIC, NULL, NULL, NULL);
    printf("%d %d\n", codec_ctx->pix_fmt, AV_PIX_FMT_YUV420P);
    int size = avpicture_get_size(AV_PIX_FMT_YUV420P, codec_ctx->width,
            codec_ctx->height);
    uint8_t* picture_buffer = (uint8_t*) (av_malloc(size));
    AVFrame* picture = av_frame_alloc();
    AVFrame* picture_rgb = av_frame_alloc();
    int size2 = avpicture_get_size(AV_PIX_FMT_RGB24, codec_ctx->width,
            codec_ctx->height);
    uint8_t* picture_buffer_2 = (uint8_t*) (av_malloc(size2));
    avpicture_fill((AVPicture *) picture, picture_buffer, AV_PIX_FMT_YUV420P,
            codec_ctx->width, codec_ctx->height);
    avpicture_fill((AVPicture *) picture_rgb, picture_buffer_2, AV_PIX_FMT_RGB24,
            codec_ctx->width, codec_ctx->height);
    unsigned int pts = 0;
    char* calc_diff;
    unsigned int num_diff = 0;
    printf("flush %d\n",avformat_flush (format_ctx));
    printf("flush %d\n",av_read_play (format_ctx));
    while (av_read_frame(format_ctx, packet) >= 0) { //read ~ 1000 frames

        if (packet->stream_index == video_stream_index) {    //packet is video
            pthread_mutex_lock(&lock); 
            num_packet++;
            int check = 0;
            int result = avcodec_decode_video2(codec_ctx, picture, &check, packet);
            if (stream == NULL) {    //create stream in file
          //      std::cout << "3 create stream" << std::endl;
                stream = avformat_new_stream(oc,format_ctx->streams[video_stream_index]->codec->codec);
                
                avcodec_copy_context(stream->codec,format_ctx->streams[video_stream_index]->codec);
                stream->sample_aspect_ratio = format_ctx->streams[video_stream_index]->codec->sample_aspect_ratio;
                avformat_write_header(oc,NULL);
                printf("timebase %d %d\n", stream->time_base.num,stream->time_base.den);
                record_time = RECORD_TIME * stream->time_base.den / stream->time_base.num / 1000;
                record_early = RECORD_EARLY * stream->time_base.den / stream->time_base.num / 1000;
                pts = 0;
                clock_gettime(CLOCK_MONOTONIC, &t1);
                printf("size %d %d %d\n", codec_ctx->height,codec_ctx->width, picture->linesize[0]);
                calc_diff = (char*) malloc(codec_ctx->height*picture->linesize[0] 
                                         //+ codec_ctx->height/2*picture->linesize[1] + 
                                          //codec_ctx->height/2*picture->linesize[2]
                                          );
                memcpy(calc_diff, picture->data[0], codec_ctx->height*picture->linesize[0]);
//                memcpy(calc_diff +codec_ctx->height*picture->linesize[0], 
  //                     picture->data[1], codec_ctx->height/2*picture->linesize[1]);
    //            memcpy(calc_diff +codec_ctx->height*picture->linesize[0] + codec_ctx->height/2*picture->linesize[1],
      //                 picture->data[2], codec_ctx->height/2*picture->linesize[2]);
            }
            else
            {
                clock_gettime(CLOCK_MONOTONIC, &t2);
                int timediff_ms = (t2.tv_sec - t1.tv_sec)*1000 + (t2.tv_nsec - t1.tv_nsec)/1000000;
                int timediff = timediff_ms * stream->time_base.den / stream->time_base.num / 1000;
                pts += timediff;
                t1 = t2;
                /* calc diff */
                int diff = 0;
                for (int y = 0; y < codec_ctx->height; y++)
                    for (int x = 0; x < codec_ctx->width; x++)
                    {
                        if (x > BLACK_W/2 || y > BLACK_H/2)
                        {
                            int d;
                            int i = y*picture->linesize[0]+x;
                            d = (unsigned char)picture->data[0][i] > (unsigned char)calc_diff[i] ? (unsigned char)picture->data[0][i] - (unsigned char)calc_diff[i] : (unsigned char)calc_diff[i] - (unsigned char)picture->data[0][i];
                            if (d>Y_CHANGE_LEVEL)
                                diff ++;
                        }
                    }
                if (diff > NUM_DIFF_PIX)
                {
//                    memcpy(calc_diff +codec_ctx->height*picture->linesize[0], 
  //                         picture->data[1], codec_ctx->height/2*picture->linesize[1]);
    //                memcpy(calc_diff +codec_ctx->height*picture->linesize[0] + codec_ctx->height/2*picture->linesize[1],
      //                     picture->data[2], codec_ctx->height/2*picture->linesize[2]);
                    
                    num_diff++;
                    //printf("diff %d\n", diff);
                    if (num_diff >= CONTINUOUS_DIFF)
                    {
                        printf("diff %d\n", diff);
                        memcpy(calc_diff, picture->data[0], codec_ctx->height*picture->linesize[0]);
                        if (!write_to_time)
                        {
                            printf("start write \n");
                            pthread_t tid1;
                            write_to_time = pts + record_time;
                            pthread_create(&tid1, NULL, write_file, NULL); 
                        }
                        else
                        {
                            write_to_time = pts + record_time;
                        }
                    }
                }
                else
                {
                    num_diff = 0;
                }
                last_node = last_node->next;
            }
            packet->pts = packet->dts = pts;
            last_node->next = (avpacket_node*)malloc(sizeof(struct avpacket_node));
            packet = &last_node->next->packet;
            av_init_packet(packet);
            if (num_packet > 1)
            {
                while (last_node->packet.pts - first_node->packet.pts > record_early)
                {
                    struct avpacket_node *next = first_node->next;
                    av_free_packet(&first_node->packet);
                    free(first_node);
                    first_node = next;
                    num_packet--;
                }
            }
            pthread_cond_signal(&cond1);
            
           
            
            
                
/*            if (cnt == 10)
            {
                for (int y = 0; y < BLACK_H; y++)
                {
                    for (int x = 0; x < BLACK_W; x++)
                    {
                        (picture->data[0]+ y * picture->linesize[0])[x] = 0;
                    }             
                }
                sws_scale(img_convert_ctx, picture->data, picture->linesize, 0,
                        codec_ctx->height, picture_rgb->data, picture_rgb->linesize);
                std::stringstream file_name;
                file_name << "test" << cnt << ".ppm";
                output_file.open(file_name.str().c_str());
                output_file << "P3 " << codec_ctx->width << " " << codec_ctx->height
                        << " 255\n";
                for (int y = 0; y < codec_ctx->height; y++) {
                    for (int x = 0; x < codec_ctx->width * 3; x++)
                        output_file
                                << (int) (picture_rgb->data[0]
                                        + y * picture_rgb->linesize[0])[x] << " ";
                }
                output_file.close();
            }
  */
            if(kbhit())
            {
                break;
            }
            pthread_mutex_unlock(&lock);
        }

    }
    av_free(picture);
    printf("done1\n");
    av_free(picture_rgb);
    printf("done1\n");
    av_free(picture_buffer);
    printf("done1\n");
    av_free(picture_buffer_2);
   printf("done1\n");
 
 //   av_read_pause(format_ctx);
 avformat_close_input(&format_ctx);
    printf("done1\n");
    av_write_trailer(oc);
    avio_close(oc->pb);
    avformat_free_context(oc);
    avio_close(output_ctx->pb);
    avformat_free_context(output_ctx);
    pthread_mutex_destroy(&lock);
    return (EXIT_SUCCESS);
}
