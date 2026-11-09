#define FM1_HOST_TEST 1
#include "../app/undertone.c"
#include <stdio.h>
#include <string.h>
#include <math.h>
static void send(uint8_t a, uint8_t b, uint8_t c){ uint8_t m[3]={a,b,c}; undertone_midi_accept(m,3); }
static FILE *wav; static long frames;
static void render(float seconds){
  long n=(long)(seconds*44100/128);
  for(long i=0;i<n;i++){ int32_t buf[256]; memset(buf,0,sizeof buf); undertone_render_add(buf);
    for(int j=0;j<256;j+=2){ int16_t s=(int16_t)(buf[j]>>8); fwrite(&s,2,1,wav);} frames+=128; }
}
static void wav_open(const char*p){ wav=fopen(p,"wb"); frames=0; fseek(wav,44,SEEK_SET);}
static void wav_close(void){ uint32_t d=frames*2; uint8_t h[44]; memcpy(h,"RIFF",4); uint32_t r=36+d; memcpy(h+4,&r,4);
 memcpy(h+8,"WAVEfmt ",8); uint32_t x=16; memcpy(h+16,&x,4); uint16_t f=1,ch=1; memcpy(h+20,&f,2); memcpy(h+22,&ch,2);
 uint32_t sr=44100,br=88200; memcpy(h+24,&sr,4); memcpy(h+28,&br,4); uint16_t ba=2,bp=16; memcpy(h+32,&ba,2); memcpy(h+34,&bp,2);
 memcpy(h+36,"data",4); memcpy(h+40,&d,4); fseek(wav,0,SEEK_SET); fwrite(h,1,44,wav); fclose(wav);}
static const char*NM[12]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static const int CMAJ[7]={60,62,64,65,67,69,71};
static void table(int mode,const char*label){
  undertone_render_add((int32_t[256]){0}); /* init */
  send(0xBB,27,mode*43); send(0xBB,22,0); render(0.0f); process_events();
  printf("%s (subs f/3, f/5, close voicing):\n",label);
  for(int i=0;i<7;i++){ float a=sub_pitch(CMAJ[i],3), b=sub_pitch(CMAJ[i],5);
    printf("  %-2s -> %-2s %+6.2f  %-2s %+6.2f\n",NM[CMAJ[i]%12],NM[((int)lrintf(a))%12],a-CMAJ[i],NM[((int)lrintf(b))%12],b-CMAJ[i]); }
}
int main(){
  table(2,"SCALE mode, C major"); table(0,"JI mode"); 
  /* demo 1: scale mode, C major scale, subs 3,5 */
  send(0xBB,120,0); send(0xBB,27,127); send(0xBB,22,0);
  wav_open("01_scale_mode_c_major.wav");
  for(int i=0;i<7;i++){ send(0x9B,CMAJ[i],100); render(0.9f); send(0x8B,CMAJ[i],0); render(0.15f);} send(0x9B,72,100); render(1.2f); send(0x8B,72,0); render(1.0f); wav_close();
  /* demo 2: JI planing, same line */
  send(0xBB,120,0); send(0xBB,27,0);
  wav_open("02_ji_planing_same_line.wav");
  for(int i=0;i<7;i++){ send(0x9B,CMAJ[i],100); render(0.9f); send(0x8B,CMAJ[i],0); render(0.15f);} send(0x9B,72,100); render(1.2f); send(0x8B,72,0); render(1.0f); wav_close();
  /* demo 3: scale mode, dorian on D, triangle + fold sweep, the West end */
  send(0xBB,120,0); send(0xBB,27,127); send(0xBB,28,2*11); send(0xBB,29,2*11); send(0xBB,70,127); send(0xBB,74,70); send(0xBB,71,60);
  wav_open("03_west_fold_sweep_d_dorian.wav");
  int line[8]={50,53,55,57,60,57,55,53};
  for(int i=0;i<8;i++){ send(0xBB,78,i*18); send(0x9B,line[i],110); render(0.6f); send(0x8B,line[i],0); render(0.1f);} render(1.0f); wav_close();
  return 0; }
