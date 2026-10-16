"""Host regressions for worker ownership, repeated inference and error recovery.
Run: python3 -m unittest discover -s test -p 'test_*.py'
Uses lightweight SDK stubs; does not validate ESP-DL or the camera hardware.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

STUB = r"""
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
#define PIXFORMAT_RGB565 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_INVALID_ARG 4
const char *esp_err_to_name(int);
typedef struct { uint8_t *buf; size_t len; size_t width,height; int format; } camera_fb_t;
camera_fb_t *esp_camera_fb_get(void);
void esp_camera_fb_return(camera_fb_t *);
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_malloc(size_t,int);
int64_t esp_timer_get_time(void);
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define portMAX_DELAY 0xffffffff
QueueHandle_t xQueueCreate(unsigned,size_t);
int xQueueReceive(QueueHandle_t,void *,unsigned);
int xQueueSend(QueueHandle_t,const void *,unsigned);
void vQueueDelete(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
bool frame2jpg(camera_fb_t *,int,uint8_t **,size_t *);
"""

HARNESS = r"""
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "processing.c"
static jmp_buf stop;
static bool queued, locked, fail_alloc, fail_camera, fail_jpeg, fail_detection;
static job_t pending;
static void (*run_worker)(void *);
static int captures, returns, detections;
static uint8_t pixels[8] = {1,2,3,4,5,6,7,8};
static camera_fb_t camera = { .buf=pixels,.len=8,.width=2,.height=2 };
const char *esp_err_to_name(int e){return "TEST_ERROR";}
QueueHandle_t xQueueCreate(unsigned n,size_t s){return (void *)1;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void *)1;}
int xSemaphoreTake(SemaphoreHandle_t l,unsigned t){assert(!locked);locked=true;return 1;}
int xSemaphoreGive(SemaphoreHandle_t l){assert(locked);locked=false;return 1;}
void vSemaphoreDelete(SemaphoreHandle_t l){}
void vQueueDelete(QueueHandle_t q){}
int xQueueSend(QueueHandle_t q,const void *p,unsigned t){if(queued)return 0;pending=*(const job_t *)p;queued=true;return 1;}
int xQueueReceive(QueueHandle_t q,void *p,unsigned t){if(!queued)longjmp(stop,1);*(job_t *)p=pending;queued=false;return 1;}
int xTaskCreate(void (*f)(void *),const char *n,unsigned s,void *a,unsigned p,void *h){run_worker=f;return 1;}
void *heap_caps_malloc(size_t n,int caps){return fail_alloc?NULL:malloc(n);}
int64_t esp_timer_get_time(void){static int64_t t;return t+=1000;}
camera_fb_t *esp_camera_fb_get(void){captures++;return fail_camera?NULL:&camera;}
void esp_camera_fb_return(camera_fb_t *f){assert(f==&camera);returns++;}
bool frame2jpg(camera_fb_t *f,int q,uint8_t **out,size_t *n){if(fail_jpeg)return false;*n=f->len;*out=malloc(*n);memcpy(*out,f->buf,*n);return true;}
esp_err_t detection_run(camera_fb_t *f,float t,detection_results_t *r){
 detections++;assert(memcmp(f->buf,pixels,8)==0);assert(f->buf!=saved.buf);
 if(fail_detection)return ESP_FAIL;
 r->count=r->total=1;r->objects[0].name="person";r->objects[0].score=t;return ESP_OK;
}
void detection_draw(camera_fb_t *f,const detection_results_t *r){f->buf[0]=99;}
static void finish(void){if(!setjmp(stop))run_worker(NULL);assert(!locked);assert(!state.busy);}
static void image_is(uint8_t first){uint8_t *data;size_t n;assert(processing_copy_jpeg(&data,&n)==ESP_OK);assert(n==8&&data[0]==first);free(data);}
int main(void){
 assert(processing_init()==ESP_OK);
 assert(processing_submit(true,.25f)==ESP_ERR_INVALID_STATE);
 assert(processing_submit(false,NAN)==ESP_ERR_INVALID_ARG);
 uint8_t *data;size_t n;
 assert(processing_copy_jpeg(&data,&n)==ESP_ERR_NOT_FOUND);
 assert(processing_submit(false,.25f)==ESP_OK);
 assert(processing_submit(false,.25f)==ESP_ERR_INVALID_STATE);
 finish();assert(captures==2&&returns==2&&detections==0);assert(state.image_id==1&&!state.inferred);image_is(1);
 for(int i=0;i<3;i++){
  assert(processing_submit(true,.25f+i*.1f)==ESP_OK);finish();image_is(99);
  assert(state.inferred&&state.detections.count==1);assert(memcmp(saved.buf,pixels,8)==0);
 }
 assert(captures==2&&detections==3);
 unsigned image_id=state.image_id;
 fail_alloc=true;assert(processing_submit(true,.25f)==ESP_OK);finish();fail_alloc=false;
 assert(state.error[0]&&state.image_id==image_id&&state.detections.count==1);image_is(99);
 fail_detection=true;assert(processing_submit(true,.25f)==ESP_OK);finish();fail_detection=false;
 assert(state.error[0]&&state.image_id==image_id);image_is(99);
 fail_jpeg=true;assert(processing_submit(false,.25f)==ESP_OK);finish();fail_jpeg=false;
 assert(state.error[0]&&state.image_id==image_id);image_is(99);
 fail_camera=true;assert(processing_submit(false,.25f)==ESP_OK);finish();fail_camera=false;
 assert(state.error[0]&&state.image_id==image_id);image_is(99);
 assert(processing_submit(false,.25f)==ESP_OK);finish();
 assert(!state.error[0]&&!state.inferred&&state.detections.count==0);image_is(1);
 uint8_t *upload=malloc(PROCESSING_IMAGE_BYTES);memset(upload,42,PROCESSING_IMAGE_BYTES);
 assert(processing_submit_image(upload,1)==ESP_ERR_INVALID_ARG);
 assert(processing_submit_image(upload,PROCESSING_IMAGE_BYTES)==ESP_OK);
 assert(processing_submit(false,.25f)==ESP_ERR_INVALID_STATE);
 finish();assert(saved.buf==upload&&saved.width==320&&saved.height==320);
 assert(!state.inferred&&state.detections.count==0&&latest_length==PROCESSING_IMAGE_BYTES);
 assert(latest_jpeg[0]==42);
 free(saved.buf);free(latest_jpeg);
 return 0;
}
"""

class ProcessingTests(unittest.TestCase):
    def test_worker_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / 'stub.h').write_text(STUB)
            for name in ['esp_camera.h', 'esp_err.h', 'esp_heap_caps.h', 'esp_timer.h',
                         'img_converters.h', 'freertos/FreeRTOS.h', 'freertos/queue.h',
                         'freertos/semphr.h', 'freertos/task.h']:
                header = tmp / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "stub.h"\n')
            (tmp / 'harness.c').write_text(HARNESS)
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                            '-Werror', '-I'+str(tmp), '-I'+str(ROOT / 'src'), '-I'+str(ROOT / 'include'),
                            str(tmp / 'harness.c'), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True)


class ModelScriptTests(unittest.TestCase):
    def run_script(self, tmp, table):
        (tmp / 'partitions.csv').write_text(table)
        class Env:
            flags = None
            validate = None
            def subst(self, name):
                return str(tmp)
            def BoardConfig(self):
                return {'build.partitions': 'partitions.csv'}
            def VerboseAction(self, *args):
                return args
            def Command(self, *args):
                return [args[0]]
            def Depends(self, *args):
                pass
            def AddPostAction(self, target, action):
                self.validate = action
            def Append(self, **kwargs):
                self.flags = kwargs['UPLOADERFLAGS']
        env = Env()
        scope = {'env': env, 'Import': lambda name: None}
        exec(compile((ROOT / 'scripts/coco_model.py').read_text(), 'coco_model.py', 'exec'), scope)
        return env

    def test_partition_offset_and_size(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            env = self.run_script(tmp, '# example\ncoco_det,data,0x40,0x510000,1K\n')
            self.assertEqual(env.flags[0], '0x510000')
            packed = tmp / 'coco_detect.espdl'
            packed.write_bytes(b'x' * 1024)
            env.validate([packed], [], env)
            packed.write_bytes(b'x' * 1025)
            with self.assertRaisesRegex(RuntimeError, 'exceeds'):
                env.validate([packed], [], env)

    def test_missing_offset_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, 'explicit offset'):
                self.run_script(pathlib.Path(directory), 'coco_det,data,0x40,,4M\n')

if __name__ == '__main__':
    unittest.main()
