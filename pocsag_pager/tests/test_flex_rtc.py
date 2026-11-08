#!/usr/bin/env python3
"""Sanitizer tests for the actual FLEX BIW decoder and continuous RTC updater.

Run in a firmware checkout: python3 tests/test_flex_rtc.py [pager directory]
Uses the firmware's datetime implementation; mocks only hardware and UI glue.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from test_flex_recovery import SHIMS, TESTS, without_includes

RTC_SHIMS = r'''
static unsigned rtc_writes,rtc_reads;
static DateTime rtc_value;
static void furi_hal_rtc_get_datetime(DateTime* value) { *value=rtc_value; rtc_reads++; }
static void furi_hal_rtc_set_datetime(DateTime* value) {
    assert(datetime_validate_datetime(value)); rtc_value=*value; rtc_writes++;
}
static uint32_t furi_kernel_get_tick_frequency(void) { return 1000; }
enum {PCSGTxRxStateRx=1};
typedef struct { FuriString* name; } TestPreset;
typedef struct {
    unsigned txrx_state;
    TestPreset* preset;
    SubGhzProtocolDecoderFlex* flex_sampler;
} TestTxRx;
typedef struct {
    struct { PCSGTimeSync time_sync; } filter;
    TestTxRx* txrx;
    uint32_t time_sync_sequence;
} POCSAGPagerApp;
static bool flex_sampler_get_frame_timing(SubGhzProtocolDecoderFlex* d,FlexFrameTiming* t) {
    return subghz_protocol_decoder_flex_get_frame_timing(d,t);
}
'''

RTC_TESTS = r'''
static void publish(SubGhzProtocolDecoderFlex* d,unsigned cycle,unsigned frame) {
    d->cycle=cycle; d->frame=frame; flex_publish_timing(d);
}
static void make_clock(uint32_t words[88],unsigned hour,bool standard,unsigned subtype) {
    memset(words,0,88*sizeof(*words));
    words[0]=encode(checksum((3U<<10)|(2U<<8))); // two auxiliary BIWs, no pages
    words[1]=encode(checksum((2U<<4)|(hour<<7)));
    words[2]=encode(checksum((5U<<4)|(subtype<<7)|(standard?1U<<20:0)));
}
static void check_snapshot(SubGhzProtocolDecoderFlex* d,bool valid,unsigned hour,bool standard) {
    FlexFrameTiming t;
    assert(subghz_protocol_decoder_flex_get_frame_timing(d,&t));
    assert(t.clock_valid==valid);
    if(valid) assert(t.hour==hour && t.standard_time==standard);
}
static void clock_words(void) {
    uint32_t words[88];
    for(unsigned hour=0;hour<32;hour++)
    for(unsigned standard=0;standard<2;standard++)
    for(unsigned subtype=4;subtype<=5;subtype++)
    for(unsigned baud=1600;baud<=3200;baud+=1600)
    for(unsigned levels=2;levels<=4;levels+=2) {
        SubGhzProtocolDecoderFlex* d=begin(baud,levels,true);
        publish(d,7,50); make_clock(words,hour,standard,subtype);
        transmit(d,words,2816*(baud==3200?2:1));
        check_snapshot(d,hour<24,hour,standard);
        assert(!callbacks && !d->partial);
        subghz_protocol_decoder_flex_free(d); cases++;
    }
    // BIW checksum, auxiliary checksum, unsupported type and untrusted FIW.
    for(unsigned bad=0;bad<5;bad++) {
        SubGhzProtocolDecoderFlex* d=begin(1600,2,false);
        d->frame_chase=bad==4; publish(d,7,50); make_clock(words,13,false,4);
        if(bad<3) words[bad]=encode((words[bad]&0x1fffff)^1U);
        if(bad==3) words[2]=encode(checksum((5U<<4)|(3U<<7)));
        transmit(d,words,2816);
        check_snapshot(d,false,0,false);
        subghz_protocol_decoder_flex_free(d); cases++;
    }
    // Neither BIW order nor a separate frame for each field should matter.
    for(unsigned reverse=0;reverse<2;reverse++) {
        SubGhzProtocolDecoderFlex* d=begin(1600,2,false); publish(d,7,50);
        uint32_t decoded[88]={0}; bool valid[88]={true,true}; bool chased[88]={0};
        uint32_t hour=checksum((2U<<4)|(13U<<7)), zone=checksum((5U<<4)|(4U<<7));
        decoded[1]=reverse?zone:hour;
        flex_decode_clock(d,decoded,valid,chased,2);
        check_snapshot(d,false,0,false);
        publish(d,7,51);
        decoded[1]=reverse?hour:zone;
        flex_decode_clock(d,decoded,valid,chased,2);
        check_snapshot(d,true,13,false);
        subghz_protocol_decoder_flex_reset_frame_timing(d);
        FlexFrameTiming t;
        assert(!subghz_protocol_decoder_flex_get_frame_timing(d,&t) && !t.clock_valid);
        subghz_protocol_decoder_flex_free(d); cases++;
    }
    // Chase-corrected structure or clock BIWs are excluded from RTC control.
    for(unsigned index=0;index<3;index++) {
        SubGhzProtocolDecoderFlex* d=begin(1600,2,true); publish(d,7,50);
        uint32_t decoded[88]={0}; bool valid[88]={true,true,true},chased[88]={0};
        decoded[1]=checksum((2U<<4)|(13U<<7)); decoded[2]=checksum((5U<<4)|(4U<<7));
        chased[index]=true;
        flex_decode_clock(d,decoded,valid,chased,3);
        check_snapshot(d,false,0,false);
        subghz_protocol_decoder_flex_free(d); cases++;
    }
    // A completed prefix may carry clock information; an unfinished word cannot.
    SubGhzProtocolDecoderFlex* d=begin(1600,2,false); publish(d,7,50);
    make_clock(words,13,true,4); transmit(d,words,256);
    flex_decode_data(d,8); check_snapshot(d,true,13,true);
    subghz_protocol_decoder_flex_reset_frame_timing(d); publish(d,7,51);
    bool valid[88]={true,true,false},chased[88]={0};
    words[1]=checksum((2U<<4)|(13U<<7)); words[2]=checksum((5U<<4)|(4U<<7));
    flex_decode_clock(d,words,valid,chased,3); check_snapshot(d,false,0,false);
    subghz_protocol_decoder_flex_free(d); cases++;
}
static void clock_rollover(void) {
    SubGhzProtocolDecoderFlex* d=begin(1600,2,false);
    publish(d,14,127);
    d->timing_clock=23|FLEX_CLOCK_HAVE_HOUR|FLEX_CLOCK_HAVE_ZONE|FLEX_CLOCK_STANDARD;
    test_tick+=1875; publish(d,0,0); check_snapshot(d,true,0,true);
    publish(d,0,1); check_snapshot(d,true,0,true);
    test_tick+=3600000; publish(d,0,2); check_snapshot(d,false,0,false);
    subghz_protocol_decoder_flex_free(d); test_tick=1000; cases++;
}
static unsigned rounded_second(unsigned cycle,unsigned frame,unsigned age) {
    return (cycle*240000+frame*1875+age+500)/1000;
}
static void rtc_modes(void) {
    SubGhzProtocolDecoderFlex* d=begin(1600,2,false);
    TestPreset preset={.name=furi_string_alloc()}; furi_string_set_str(preset.name,"FLEX");
    TestTxRx txrx={PCSGTxRxStateRx,&preset,d};
    POCSAGPagerApp app={.txrx=&txrx};
    DateTime original={.hour=6,.minute=0,.second=0,.day=8,.month=11,.year=2026,.weekday=7};
    const unsigned cycles[]={0,7,14},frames[]={0,9,10,50,127},ages[]={0,1875,5000};
    for(unsigned mode=0;mode<3;mode++)
    for(unsigned ready=0;ready<2;ready++)
    for(unsigned standard=0;standard<2;standard++)
    for(unsigned h=0;h<24;h++)
    for(unsigned c=0;c<COUNT_OF(cycles);c++)
    for(unsigned f=0;f<COUNT_OF(frames);f++)
    for(unsigned a=0;a<COUNT_OF(ages);a++) {
        app.filter.time_sync=mode; app.time_sync_sequence=0;
        test_tick=1000; d->frame_chase=false;
        subghz_protocol_decoder_flex_reset_frame_timing(d);
        publish(d,cycles[c],frames[f]);
        d->timing_clock=h|(ready?(FLEX_CLOCK_HAVE_HOUR|FLEX_CLOCK_HAVE_ZONE):0)|
                           (standard?FLEX_CLOCK_STANDARD:0);
        test_tick+=ages[a]; rtc_value=original; rtc_writes=rtc_reads=0;
        pcsg_time_sync(&app);
        unsigned gps=rounded_second(cycles[c],frames[f],ages[a]);
        // Reference: start at the BIW's local daylight hour, add frame elapsed
        // seconds, subtract the GPS offset and standard-time hour, wrap a day.
        int local=(h*3600+gps-18-(standard?3600:0)+86400)%86400;
        if(mode==PCSGTimeSyncOff) {
            assert(!rtc_writes && !rtc_reads);
        } else if(mode==PCSGTimeSyncFull && ready) {
            int expected=abs(local-6*3600)<2?6*3600:local;
            assert(rtc_value.hour==expected/3600);
            assert(rtc_value.minute==(expected%3600)/60 && rtc_value.second==expected%60);
            assert(rtc_value.year==original.year && rtc_value.month==original.month &&
                   rtc_value.day==original.day && rtc_value.weekday==original.weekday);
        } else {
            int second=(gps+3600-18)%3600;
            int delta=second;
            if(delta>1800) delta-=3600;
            if(abs(delta)<2) delta=0;
            DateTime expected;
            datetime_timestamp_to_datetime(datetime_datetime_to_timestamp(&original)+delta,&expected);
            assert(rtc_value.hour==expected.hour && rtc_value.minute==expected.minute &&
                   rtc_value.second==expected.second);
        }
        unsigned writes=rtc_writes,reads=rtc_reads;
        pcsg_time_sync(&app); assert(rtc_writes==writes && rtc_reads==reads);
        cases++;
    }
    // A subsequent fresh frame must still correct a changed/drifting RTC.
    app.filter.time_sync=PCSGTimeSyncFull;
    for(unsigned n=0;n<3;n++) {
        test_tick=1000+n*1875; publish(d,7,50+n);
        d->timing_clock=13|FLEX_CLOCK_HAVE_HOUR|FLEX_CLOCK_HAVE_ZONE;
        rtc_value=original; rtc_writes=0;
        pcsg_time_sync(&app); assert(rtc_writes==1 && rtc_value.hour==13); cases++;
    }
    // Fresh first FIW synchronizes MM:SS; later BIWs set the hour in the same frame.
    subghz_protocol_decoder_flex_reset_frame_timing(d); publish(d,7,50);
    rtc_value=original; pcsg_time_sync(&app); assert(rtc_value.hour==6);
    uint32_t words[88]={0}; bool valid[88]={true,true,true},chased[88]={0};
    words[1]=checksum((2U<<4)|(13U<<7)); words[2]=checksum((5U<<4)|(4U<<7)|(1U<<20));
    flex_decode_clock(d,words,valid,chased,3); pcsg_time_sync(&app);
    assert(rtc_value.hour==12); cases++;

    // Stale or Chase FIWs, a non-FLEX preset and inactive RX never write.
    for(unsigned bad=0;bad<4;bad++) {
        test_tick=1000; d->frame_chase=bad==1; publish(d,7,50);
        if(bad==0) test_tick+=5001;
        furi_string_set_str(preset.name,bad==2?"POCSAG":"FLEX4");
        txrx.txrx_state=bad==3?0:PCSGTxRxStateRx;
        app.time_sync_sequence=0; rtc_reads=rtc_writes=0;
        pcsg_time_sync(&app); assert(!rtc_reads && !rtc_writes); cases++;
    }
    txrx.txrx_state=PCSGTxRxStateRx; d->frame_chase=false;
    // Existing MM:SS behavior carries across midnight and keeps running.
    app.filter.time_sync=PCSGTimeSyncMinuteSecond; test_tick=1000;
    publish(d,0,11); rtc_value=original; rtc_value.hour=23; rtc_value.minute=59; rtc_value.second=59;
    pcsg_time_sync(&app);
    assert(rtc_value.hour==0 && rtc_value.minute==0 && rtc_value.second==3 && rtc_value.day==9);
    publish(d,0,12); rtc_value=original; rtc_writes=0;
    pcsg_time_sync(&app); assert(rtc_writes==1); cases++;
    furi_string_free(preset.name); subghz_protocol_decoder_flex_free(d); test_tick=1000;
}
int main(void) {
    clock_words(); clock_rollover(); rtc_modes();
    printf("PASS: %u RTC cases: BIWs, BCH/checksum/Chase gates, all modes, hour rollover,\n"
           "standard/daylight time, GPS borrow, fresh/stale frames, date carry and continuous correction.\n",cases);
}
'''

def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    protocol = root / 'protocols'
    firmware = root.parents[2]
    shims = SHIMS.replace('static uint32_t furi_get_tick(void) { return 1000; }',
                          'static uint32_t test_tick=1000;\nstatic uint32_t furi_get_tick(void) { return test_tick; }')
    source = (protocol / 'flex.c').read_text()
    source = source[:source.index('SubGhzProtocolStatus subghz_protocol_decoder_flex_serialize(')]
    unit = shims + without_includes((protocol / 'flex.h').read_text())
    unit += without_includes((protocol / 'flex_bch.h').read_text())
    unit += without_includes((protocol / 'flex_bch.c').read_text())
    unit += without_includes(source) + TESTS[:TESTS.index('int main(void)')]
    unit += '\n#define furi_check assert\n#undef TAG\n'
    unit += without_includes((firmware / 'lib/datetime/datetime.h').read_text())
    unit += without_includes((firmware / 'lib/datetime/datetime.c').read_text())
    settings = (root / 'pocsag_pager_settings.h').read_text()
    unit += re.search(r'typedef enum \{\s*PCSGTimeSyncOff,.*?\} PCSGTimeSync;', settings, re.S)[0]
    unit += RTC_SHIMS + without_includes((root / 'pocsag_pager_time.c').read_text()) + RTC_TESTS
    with tempfile.TemporaryDirectory(prefix='flex-rtc-') as tmp:
        path = Path(tmp)
        (path / 'test.c').write_text(unit)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)

if __name__ == '__main__':
    main()
