#!/usr/bin/env python3
"""Host regression test for FLEX block recovery; requires Python 3 and a C compiler.

Run: python3 test_flex_recovery.py /path/to/pocsag_pager
Compiles the actual decoder/BCH source with host shims for strings, ticks and
callbacks. Hardware and persistence glue are excluded. No radio/device access.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

SHIMS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#define UNUSED(x) (void)(x)
#define COUNT_OF(x) (sizeof(x)/sizeof((x)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define furi_assert assert
#define FURI_LOG_D(...) ((void)0)
#define FURI_LOG_E(...) ((void)0)
typedef struct { char data[4096]; } FuriString;
static FuriString* furi_string_alloc(void) { return calloc(1,sizeof(FuriString)); }
static void furi_string_free(FuriString* s) { free(s); }
static void furi_string_reset(FuriString* s) { s->data[0]=0; }
static size_t furi_string_size(const FuriString* s) { return strlen(s->data); }
static const char* furi_string_get_cstr(const FuriString* s) { return s->data; }
static char furi_string_get_char(const FuriString* s, size_t i) { return s->data[i]; }
static void furi_string_set_str(FuriString* s,const char* p) {
    assert(strlen(p)<sizeof(s->data)); strcpy(s->data,p);
}
static void furi_string_cat_str(FuriString* s,const char* p) {
    assert(strlen(s->data)+strlen(p)<sizeof(s->data)); strcat(s->data,p);
}
static void furi_string_cat(FuriString* s,const FuriString* p) { furi_string_cat_str(s,p->data); }
static void furi_string_push_back(FuriString* s,char c) {
    size_t n=strlen(s->data); assert(n+1<sizeof(s->data)); s->data[n]=c; s->data[n+1]=0;
}
static void furi_string_printf(FuriString* s,const char* fmt,...) {
    va_list ap; va_start(ap,fmt); int n=vsnprintf(s->data,sizeof(s->data),fmt,ap);
    va_end(ap); assert(n>=0 && (size_t)n<sizeof(s->data));
}
static void furi_string_cat_printf(FuriString* s,const char* fmt,...) {
    size_t len=strlen(s->data); va_list ap; va_start(ap,fmt);
    int n=vsnprintf(s->data+len,sizeof(s->data)-len,fmt,ap);
    va_end(ap); assert(n>=0 && (size_t)n<sizeof(s->data)-len);
}
static uint32_t furi_get_tick(void) { return 1000; }
static uint32_t furi_ms_to_ticks(uint32_t ms) { return ms; }
static void furi_hal_speaker_start(float hz,float volume) { (void)hz; (void)volume; }
typedef struct { const char* name; } SubGhzProtocol;
typedef struct SubGhzProtocolDecoderBase SubGhzProtocolDecoderBase;
struct SubGhzProtocolDecoderBase {
    const SubGhzProtocol* protocol;
    void (*callback)(SubGhzProtocolDecoderBase*,void*);
    void* context;
};
typedef void SubGhzEnvironment;
typedef struct {
    const char* protocol_name;
    FuriString *result_ric,*result_msg,*serialize_scratch;
} PCSGBlockGeneric;
const SubGhzProtocol subghz_protocol_flex = {.name="FLEX"};
'''

TESTS = r'''
static unsigned callbacks, phase_mask, cases;
static bool reset_on_callback;
static char last_message[4096];
static void on_page(SubGhzProtocolDecoderBase* base, void* context) {
    UNUSED(context);
    SubGhzProtocolDecoderFlex* d=(void*)base;
    callbacks++; phase_mask|=1U<<(d->phase-'A');
    strcpy(last_message,furi_string_get_cstr(d->generic.result_msg));
    if(reset_on_callback) subghz_protocol_decoder_flex_reset(d);
}
static SubGhzProtocolDecoderFlex* begin(unsigned baud,unsigned levels,bool weak) {
    SubGhzProtocolDecoderFlex* d=subghz_protocol_decoder_flex_alloc(NULL);
    d->base.callback=on_page;
    subghz_protocol_decoder_flex_set_weak_decode(d,weak);
    d->baud=baud; d->levels=levels;
    d->state=FlexDecoderStateSync2;
    d->sync2_count=baud*FLEX_SYNC2_DURATION_MS/1000-1;
    flex_process_symbol(d,0);
    assert(d->state==FlexDecoderStateData);
    callbacks=phase_mask=0; last_message[0]=0;
    return d;
}
static uint32_t encode(uint32_t data) {
    assert(data<0x200000);
    for(uint32_t parity=0;parity<1024;parity++) {
        uint32_t word=data|(parity<<21), copy=word;
        if(flex_bch_correct(&copy)==0) {
            return word|((uint32_t)(__builtin_popcount(word)&1)<<31);
        }
    }
    assert(false); return 0;
}
static uint32_t checksum(uint32_t word) {
    for(unsigned n=0;n<16;n++) if(flex_word_checksum_valid(word|n)) return word|n;
    assert(false); return 0;
}
static void make_binary(uint32_t words[88],unsigned first) {
    memset(words,0,88*sizeof(*words));
    words[0]=encode(checksum(2U<<10));
    words[1]=encode(0x8000U+1234);
    words[2]=encode(checksum((FlexPageTypeBinary<<4)|(first<<7)|(1U<<14)));
    words[first]=encode(0x12345);
}
// Feed the transmitted bit order, including the A/C symbol alternation.
static void transmit(SubGhzProtocolDecoderFlex* d,const uint32_t words[88],unsigned symbols) {
    for(unsigned n=0;n<symbols;n++) {
        unsigned bit=d->data_bit_count;
        unsigned index=(bit/256)*8+bit%8;
        unsigned shift=(bit%256)/8;
        bool value=(words[index]>>shift)&1;
        flex_process_symbol(d,value?(d->levels==4?2:3):0);
    }
}
static unsigned phases(unsigned baud,unsigned levels) {
    return (baud==3200?2:1)*(levels==4?2:1);
}
static void boundaries(void) {
    uint32_t words[88];
    for(unsigned first=7;first<88;first+=(first%8==7?1:7)) {
        make_binary(words,first);
        for(unsigned baud=1600;baud<=3200;baud+=1600)
        for(unsigned levels=2;levels<=4;levels+=2)
        for(unsigned weak=0;weak<2;weak++)
        for(unsigned block=1;block<=10;block++)
        for(int delta=-1;delta<=1;delta++) {
            SubGhzProtocolDecoderFlex* d=begin(baud,levels,weak);
            unsigned bits=block*256+delta;
            unsigned count=phases(baud,levels), received=(bits/256)*8;
            transmit(d,words,bits*(baud==3200?2:1));
            subghz_protocol_decoder_flex_handle_input_loss(d);
            assert(callbacks==(first<received?count:0));
            assert(d->stats.words_total==received*count);
            assert(d->stats.frames_completed==0);
            assert(d->stats.frames_interrupted==1);
            assert(d->stats.frames_partial==(received>0));
            assert(d->stats.words_chase==0);
            assert(d->partial==NULL); // missing tail never enters repeat repair
            if(callbacks) assert(!strcmp(last_message,"BIN: 012345"));
            unsigned before=callbacks;
            subghz_protocol_decoder_flex_handle_input_loss(d);
            assert(callbacks==before && d->stats.frames_interrupted==1);
            subghz_protocol_decoder_flex_free(d); cases++;
        }
    }
}
static void full_frames(void) {
    uint32_t words[88]; make_binary(words,87);
    for(unsigned baud=1600;baud<=3200;baud+=1600)
    for(unsigned levels=2;levels<=4;levels+=2)
    for(unsigned reset=0;reset<2;reset++) {
        SubGhzProtocolDecoderFlex* d=begin(baud,levels,false);
        reset_on_callback=reset;
        transmit(d,words,2816*(baud==3200?2:1));
        reset_on_callback=false;
        assert(callbacks==phases(baud,levels));
        assert(phase_mask==(levels==4?(baud==3200?15:3):(baud==3200?5:1)));
        assert(d->stats.frames_completed==1 && d->stats.frames_interrupted==0);
        assert(d->stats.words_total==88*phases(baud,levels));
        subghz_protocol_decoder_flex_handle_input_loss(d);
        assert(callbacks==phases(baud,levels));
        subghz_protocol_decoder_flex_free(d); cases++;
    }
}
static void timing_loss(void) {
    uint32_t words[88]; make_binary(words,7);
    for(unsigned baud=1600;baud<=3200;baud+=1600) {
        unsigned scale=baud==3200?2:1;
        for(unsigned bits=255;bits<=256;bits++) {
            SubGhzProtocolDecoderFlex* d=begin(baud,2,false);
            transmit(d,words,bits*scale);
            // 1.4 symbols: invalid timing, must not finish the first block.
            subghz_protocol_decoder_flex_feed(d,true,baud==1600?875:438);
            assert(callbacks==(bits==256?phases(baud,2):0));
            assert(d->stats.words_total==(bits==256?8*phases(baud,2):0));
            assert(d->state==FlexDecoderStateSync1);
            subghz_protocol_decoder_flex_free(d); cases++;
        }
        SubGhzProtocolDecoderFlex* d=begin(baud,2,false);
        transmit(d,words,256*scale);
        subghz_protocol_decoder_flex_feed(d,true,UINT32_MAX);
        assert(callbacks==phases(baud,2) && d->stats.frames_interrupted==1);
        subghz_protocol_decoder_flex_free(d); cases++;

        make_binary(words,87);
        d=begin(baud,2,false);
        transmit(d,words,2816*scale-1);
        subghz_protocol_decoder_flex_feed(d,true,baud==1600?875:563);
        assert(callbacks==0 && d->stats.frames_completed==0);
        assert(d->stats.words_total==80*phases(baud,2));
        subghz_protocol_decoder_flex_free(d); cases++;
        make_binary(words,7);
    }
}
static void ordinary_reset(void) {
    uint32_t words[88]; make_binary(words,7);
    SubGhzProtocolDecoderFlex* d=begin(1600,4,false);
    transmit(d,words,256);
    subghz_protocol_decoder_flex_reset(d);
    assert(callbacks==0 && d->stats.frames_interrupted==0);
    subghz_protocol_decoder_flex_handle_input_loss(d);
    assert(callbacks==0 && d->stats.frames_interrupted==0);
    subghz_protocol_decoder_flex_free(d); cases++;
}
static void make_alpha(uint32_t words[88],bool long_address,unsigned first,
                       bool continuation,unsigned fragment,uint32_t text) {
    memset(words,0,88*sizeof(*words));
    unsigned vector=long_address?3:2, header=long_address?4:first;
    words[0]=checksum(vector<<10);
    words[1]=long_address?1:0x8000U+1234;
    if(long_address) words[2]=0x1ffffe;
    words[vector]=checksum((FlexPageTypeAlphanumeric<<4)|(first<<7)|(2U<<14));
    uint32_t page[2]={(fragment<<11)|(continuation<<10),text};
    while(!flex_alpha_checksum_valid(page,0,1)) page[0]++;
    words[header]=page[0]; words[long_address?first:first+1]=page[1];
    for(unsigned i=0;i<88;i++) words[i]=encode(words[i]);
}
static void alpha_and_headers(void) {
    uint32_t words[88];
    for(unsigned long_address=0;long_address<2;long_address++)
    for(unsigned first=6;first<=8;first++) {
        make_alpha(words,long_address,first,false,3,('O'<<7)|('K'<<14));
        SubGhzProtocolDecoderFlex* d=begin(1600,2,true);
        transmit(d,words,256);
        subghz_protocol_decoder_flex_handle_input_loss(d);
        assert(callbacks==((long_address?first:first+1)<8));
        if(callbacks) assert(!strcmp(last_message,"ALN: OK"));
        assert(!d->partial && !d->stats.pages_checksum_bad);
        subghz_protocol_decoder_flex_free(d); cases++;
    }
    // Vector itself has not arrived, despite a received address and BIW.
    make_binary(words,16);
    words[0]=encode(checksum(8U<<10)); words[8]=words[2]; words[2]=0;
    SubGhzProtocolDecoderFlex* d=begin(1600,2,true);
    transmit(d,words,256);
    subghz_protocol_decoder_flex_handle_input_loss(d);
    assert(!callbacks && !d->partial && d->stats.words_total==8);
    subghz_protocol_decoder_flex_free(d); cases++;

    // Long tone's first vector is in word 7; its second slot is unreceived.
    memset(words,0,sizeof(words));
    words[0]=encode(checksum(7U<<10));
    words[1]=encode(1); words[2]=encode(0x1ffffe);
    words[7]=encode(checksum(FlexPageTypeTone<<4));
    d=begin(1600,2,true); transmit(d,words,256);
    subghz_protocol_decoder_flex_handle_input_loss(d);
    assert(!callbacks); subghz_protocol_decoder_flex_free(d); cases++;

    // A/C alternation: A has its final bit but C has not completed the block.
    make_binary(words,7); d=begin(3200,4,true); transmit(d,words,511);
    subghz_protocol_decoder_flex_handle_input_loss(d);
    assert(!callbacks && !d->stats.words_total);
    subghz_protocol_decoder_flex_free(d); cases++;
}
static void joined_fragments(void) {
    uint32_t words[88];
    unsigned signature=(~('O'+'K'+'!'))&0x7f;
    make_alpha(words,false,6,true,3,signature|('O'<<7)|('K'<<14));
    SubGhzProtocolDecoderFlex* d=begin(1600,2,true);
    transmit(d,words,256);
    subghz_protocol_decoder_flex_handle_input_loss(d);
    assert(!callbacks && d->stats.fragments_first==1);
    // The existing join survives the input-loss reset and completes normally.
    d->state=FlexDecoderStateSync2; d->sync2_count=39;
    flex_process_symbol(d,0);
    make_alpha(words,false,6,false,0,'!'|(3U<<7)|(3U<<14));
    transmit(d,words,2816);
    assert(callbacks==1 && !strcmp(last_message,"ALN: OK!"));
    assert(d->stats.joined_complete==1 && !d->stats.joined_signature_bad);
    assert(!d->stats.pages_checksum_bad && !d->page_partial);
    subghz_protocol_decoder_flex_free(d); cases++;
}

static void biw_checksum_gate(void) {
    uint32_t words[88];
    const uint32_t good=checksum(2U<<10);
    // All check nibbles are BCH-encoded: bad-check cases cannot be dismissed
    // as uncorrectable words. Exercise full frames and recovered prefixes.
    for(unsigned nibble=0;nibble<16;nibble++)
    for(unsigned baud=1600;baud<=3200;baud+=1600)
    for(unsigned levels=2;levels<=4;levels+=2)
    for(unsigned weak=0;weak<2;weak++)
    for(unsigned partial=0;partial<2;partial++) {
        make_binary(words,7);
        words[0]=encode((good&~15U)|nibble);
        SubGhzProtocolDecoderFlex* d=begin(baud,levels,weak);
        transmit(d,words,(partial?256:2816)*(baud==3200?2:1));
        if(partial) subghz_protocol_decoder_flex_handle_input_loss(d);
        bool valid=nibble==(good&15U);
        assert(callbacks==(valid?phases(baud,levels):0));
        assert(d->stats.phases_bad_biw==(valid?0:phases(baud,levels)));
        assert(!d->stats.words_bad && !d->partial);
        subghz_protocol_decoder_flex_free(d); cases++;
    }
    // Check after BCH correction, including errors in the check character.
    for(unsigned errors=1;errors<=2;errors++)
    for(unsigned partial=0;partial<2;partial++) {
        make_binary(words,7);
        words[0]^=errors==1?1U:0x401U;
        SubGhzProtocolDecoderFlex* d=begin(1600,2,false);
        transmit(d,words,partial?256:2816);
        if(partial) subghz_protocol_decoder_flex_handle_input_loss(d);
        assert(callbacks==1 && !d->stats.phases_bad_biw);
        subghz_protocol_decoder_flex_free(d); cases++;
    }
    // Reject only the bad phase; a valid phase in the same frame survives.
    make_binary(words,7);
    SubGhzProtocolDecoderFlex* d=begin(1600,4,false);
    transmit(d,words,256);
    d->phase_a[0]=encode(good^1U);
    subghz_protocol_decoder_flex_handle_input_loss(d);
    assert(callbacks==1 && phase_mask==2 && d->stats.phases_bad_biw==1);
    subghz_protocol_decoder_flex_free(d); cases++;

    // Chase recovery is also subject to the check character. Find a
    // three-bit error pattern recoverable from reliable untouched bits.
    for(unsigned bad=0;bad<2;bad++) {
        make_binary(words,7);
        words[0]=encode(good^bad);
        d=begin(1600,2,true); transmit(d,words,256);
        bool found=false;
        for(unsigned a=0;a<29 && !found;a++)
        for(unsigned b=a+1;b<30 && !found;b++)
        for(unsigned c=b+1;c<31 && !found;c++) {
            uint32_t raw=words[0]^(1U<<a)^(1U<<b)^(1U<<c), trial=raw;
            if(flex_bch_correct(&trial)>=0) continue;
            memset(d->phase_rel,255,FLEX_REL_WORD);
            d->phase_rel[a]=d->phase_rel[b]=d->phase_rel[c]=0;
            trial=raw;
            if(flex_bch_chase(&trial,d->phase_rel) && trial==words[0]) {
                d->phase_a[0]=raw; found=true;
            }
        }
        assert(found);
        subghz_protocol_decoder_flex_handle_input_loss(d);
        assert(d->stats.words_chase==1 && d->stats.phases_bad_biw==bad);
        assert(callbacks==!bad);
        subghz_protocol_decoder_flex_free(d); cases++;
    }
    // Preserve the existing silent treatment of idle BIWs.
    for(unsigned ones=0;ones<2;ones++) {
        make_binary(words,7); words[0]=encode(ones?0x1fffff:0);
        d=begin(1600,2,false); transmit(d,words,256);
        subghz_protocol_decoder_flex_handle_input_loss(d);
        assert(!callbacks && !d->stats.phases_bad_biw);
        subghz_protocol_decoder_flex_free(d); cases++;
    }
}

static void pulse_timing_equivalence(void) {
    // Compare the preflight with the original timing loop at every end-of-frame
    // offset up to the 512-symbol cap, including its 3200 -> 1600 rate switch.
    SubGhzProtocolDecoderFlex* d=begin(1600,2,false);
    for(unsigned baud=1600;baud<=3200;baud+=1600) {
        d->baud=baud;
        for(unsigned left=1;left<=513;left+=1) {
            d->data_count=baud*FLEX_DATA_DURATION_MS/1000-left;
            for(unsigned duration=0;duration<400000;duration+=337) {
                int32_t remaining=duration*2; unsigned processed=0;
                while(processed<512) {
                    unsigned period=baud==3200 && processed<left?625:1250;
                    if(remaining<(int32_t)(period/2)) break;
                    remaining-=period; processed++;
                }
                int tolerance=baud==3200 && processed<left?120:200;
                bool valid=processed!=512 && abs(remaining)<=tolerance;
                assert(flex_data_run_timing_valid(d,duration)==valid);
            }
        }
    }
    subghz_protocol_decoder_flex_free(d); cases++;
}
int main(void) {
    boundaries(); full_frames(); timing_loss(); ordinary_reset();
    alpha_and_headers(); joined_fragments(); biw_checksum_gate(); pulse_timing_equivalence();
    printf("PASS: %u recovery cases, all phase/rate modes, Weak Decode on/off, full frames,\n"
           "timing failures, header bounds, BIW checks, alpha/joins, resets, and 1.2M pulse-timing comparisons.\n",cases);
}
'''

def without_includes(source):
    return re.sub(r'^\s*#(?:include|pragma once)[^\n]*', '', source, flags=re.M)

def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    protocol = root / 'protocols'
    source = (protocol / 'flex.c').read_text()
    # Keep all decoding/recovery/joining logic; exclude unrelated persistence and vtables.
    source = source[:source.index('SubGhzProtocolStatus subghz_protocol_decoder_flex_serialize(')]
    unit = SHIMS + without_includes((protocol / 'flex.h').read_text())
    unit += without_includes((protocol / 'flex_bch.h').read_text())
    unit += without_includes((protocol / 'flex_bch.c').read_text())
    unit += '\n#line 1 "flex.c"\n' + without_includes(source) + TESTS
    with tempfile.TemporaryDirectory(prefix='flex-recovery-') as tmp:
        path = Path(tmp)
        (path / 'test.c').write_text(unit)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)

if __name__ == '__main__':
    main()
