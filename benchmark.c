/*******************************************************************************
 * benchmark.c
 *
 * A benchmark that:
 *   1. Takes either a .png file or a directory containing .png files.
 *   2. For each PNG found:
 *       a) Reads it via stb_image (=> RGBA).
 *       b) Calls qoy_rgba_to_ycbcra (純 C) multiple times, measures time.
 *       c) Calls qoy_rgba_to_ycbcra_rvv (RVV) multiple times, measures time.
 *       d) Compares outputs and prints times for that file.
 *   3. At the end, prints a "global average" across all files.
 *
 * Usage:
 *   gcc -O3 -o benchmark benchmark.c koy.c -lm -lpng -lz
 *   ./benchmark <file_or_directory> <runs>
 *
 *******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>     // For opendir, readdir

//------------------------------[ STB_IMAGE ]-----------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

//------------------------------[ QOY ]-----------------------------------------
#define QOY_IMPLEMENTATION
#include "qoy.h"

//------------------------------[ Timer ]---------------------------------------
#if defined(__APPLE__)
  #include <mach/mach_time.h>
#elif defined(__linux__)
  #include <time.h>
#elif defined(_WIN32)
  #include <windows.h>
#endif

static uint64_t ns(void) {
#if defined(__APPLE__)
    static mach_timebase_info_data_t info;
    static int init=0;
    if(!init) {
        mach_timebase_info(&info);
        init=1;
    }
    uint64_t now = mach_absolute_time();
    now = now * info.numer / info.denom;
    return now;
#elif defined(__linux__)
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    return (uint64_t)spec.tv_sec * 1000000000ULL + (uint64_t)spec.tv_nsec;
#elif defined(_WIN32)
    static LARGE_INTEGER freq;
    static int init=0;
    if(!init){
        QueryPerformanceFrequency(&freq);
        init=1;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)(1000000000ULL* now.QuadPart / freq.QuadPart);
#else
    return (uint64_t)clock();
#endif
}

// -----------------------------------------------------------------------------
// 用來累加所有圖片（C、RVV）耗時的全域變數
static uint64_t g_sum_c_time_ns   = 0;  // 所有檔案的「純 C 轉換」總時間(單位 ns)
static uint64_t g_sum_rvv_time_ns = 0;  // 所有檔案的「RVV 轉換」總時間(單位 ns)
static int      g_image_count     = 0;  // 累計測試的 PNG 檔案數量
static int      g_runs            = 1;  // 全域紀錄 runs（可選）

/* 
   benchmark_image():
   讀取 PNG => rgba；然後執行多次( runs ) 純C + RVV 轉換並計時，印出該檔結果後，
   把耗時累加到全域統計 g_sum_c_time_ns / g_sum_rvv_time_ns。
*/
static void benchmark_image(const char* path, int runs) {
    int width, height, comp;
    unsigned char* rgba = stbi_load(path, &width, &height, &comp, 4);
    if(!rgba) {
        printf("Error: failed to load PNG: %s\n", path);
        return;
    }
    printf("[File] %s => %dx%d, forced RGBA=4\n", path, width, height);

    // (1) 分配 out buffer
    int block_size = (4 == 4)? 10: 6; 
    int outbuf_size = (width>>1)*height* block_size; // 大致上線
    unsigned char* out_c   = (unsigned char*)malloc(outbuf_size);
    unsigned char* out_rvv = (unsigned char*)malloc(outbuf_size);

    // (2) 預先 warm-up(可選)
    qoy_rgba_to_ycbcra(rgba, width, height, 4, 4, out_c);
    qoy_rgba_to_ycbcra_rvv(rgba, width, height, 4, 4, out_rvv);

    // (3) 分別計時
    uint64_t sum_c=0, sum_rvv=0;
    for(int i=0; i<runs; i++){
        // C 版
        memset(out_c, 0, outbuf_size);
        uint64_t t0 = ns();
        qoy_rgba_to_ycbcra(rgba, width, height, 4, 4, out_c);
        uint64_t t1 = ns();
        sum_c += (t1 - t0);

        // RVV 版
        memset(out_rvv, 0, outbuf_size);
        uint64_t t2 = ns();
        qoy_rgba_to_ycbcra_rvv(rgba, width, height, 4, 4, out_rvv);
        uint64_t t3 = ns();
        sum_rvv += (t3 - t2);
    }

    double avg_c   = (double)sum_c   / runs / 1.0e6; // ms
    double avg_rvv = (double)sum_rvv / runs / 1.0e6;
    printf("Runs=%d | C=%.3f ms, RVV=%.3f ms\n", runs, avg_c, avg_rvv);

    // (5) 累加到全域統計 (sum_c, sum_rvv)
    g_sum_c_time_ns   += sum_c;    // sum_c 依然是 runs 次的總和(單位 ns)
    g_sum_rvv_time_ns += sum_rvv;
    g_image_count++; 

    free(rgba);
    free(out_c);
    free(out_rvv);
}

/* 
   benchmark_directory():
   遞迴(or非遞迴) 打開某個目錄，尋找 .png 檔，對每張 png 呼叫 benchmark_image()
*/
static void benchmark_directory(const char* dirpath, int runs) {
    DIR* dp = opendir(dirpath);
    if(!dp) {
        printf("Could not open directory: %s\n", dirpath);
        return;
    }

    struct dirent* ent;
    while((ent = readdir(dp)) != NULL) {
        if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, ent->d_name);

        // 檢查副檔名是不是 .png
        size_t len = strlen(ent->d_name);
        if(len>4 && strcmp(ent->d_name + (len-4), ".png")==0) {
            benchmark_image(filepath, runs);
        }
        // 若想遞迴子目錄 => if(ent->d_type==DT_DIR)...(再呼叫 benchmark_directory)
    }

    closedir(dp);
}

//------------------------------[ main ]----------------------------------------
int main(int argc, char** argv) {
    if(argc < 3) {
        printf("Usage: %s <file_or_directory> <runs>\n", argv[0]);
        return 0;
    }

    const char* input_path = argv[1];
    g_runs = atoi(argv[2]);  // 用全域紀錄
    if(g_runs <= 0) g_runs=1;

    // 先判斷 input_path 是檔案 還是資料夾
    struct stat st;
    if(stat(input_path, &st)==0) {
        if(S_ISDIR(st.st_mode)) {
            // 是資料夾 => 遍歷該資料夾
            benchmark_directory(input_path, g_runs);
        }
        else if(S_ISREG(st.st_mode)) {
            // 是檔案 => 直接當成 png
            benchmark_image(input_path, g_runs);
        }
        else {
            printf("Input path is neither file nor directory???\n");
        }
    } else {
        printf("Cannot stat: %s\n", input_path);
    }

    // ---------- 在這裡印出「整體平均」 ----------
    if(g_image_count>0) {
        // g_sum_c_time_ns, g_sum_rvv_time_ns 都是所有檔案的 "runs 次總和"
        // => 單檔 + runs => sum_c
        // => 全部 => (g_image_count*runs)
        double avg_c   = (double)g_sum_c_time_ns / (double)(g_image_count*g_runs);
        double avg_rvv = (double)g_sum_rvv_time_ns / (double)(g_image_count*g_runs);
        // 轉成毫秒
        avg_c   /= 1.0e6;
        avg_rvv /= 1.0e6;

        printf("===== Global Average across %d PNG(s) =====\n", g_image_count);
        printf("C   version: %.3f ms\n", avg_c);
        printf("RVV version: %.3f ms\n", avg_rvv);
    } else {
        printf("No PNG files were processed.\n");
    }

    return 0;
}

