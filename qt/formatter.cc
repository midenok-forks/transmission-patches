/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 * $Id: formatter.cc 13195 2012-02-03 21:21:52Z jordan $
 */

#include <iostream>

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h> // tr_formatter

#include "formatter.h"
#include "speed.h"

/***
****  Constants
***/

namespace
{
    unsigned int speed_K;
    unsigned int mem_K;
    unsigned int size_K;
}

QString Formatter::unitStrings[3][5];

void
Formatter :: initUnits( )
{
    speed_K = 1000;
    unitStrings[SPEED][B]  = tr(  "B/s" );
    unitStrings[SPEED][KB] = tr( "kB/s" );
    unitStrings[SPEED][MB] = tr( "MB/s" );
    unitStrings[SPEED][GB] = tr( "GB/s" );
    unitStrings[SPEED][TB] = tr( "TB/s" );
    tr_formatter_speed_init( speed_K,
                             qPrintable( unitStrings[SPEED][KB] ),
                             qPrintable( unitStrings[SPEED][MB] ),
                             qPrintable( unitStrings[SPEED][GB] ),
                             qPrintable( unitStrings[SPEED][TB] ) );

    size_K = 1000;
    unitStrings[SIZE][B]  = tr(  "B" );
    unitStrings[SIZE][KB] = tr( "kB" );
    unitStrings[SIZE][MB] = tr( "MB" );
    unitStrings[SIZE][GB] = tr( "GB" );
    unitStrings[SIZE][TB] = tr( "TB" );
    tr_formatter_size_init( size_K,
                            qPrintable( unitStrings[SIZE][KB] ),
                            qPrintable( unitStrings[SIZE][MB] ),
                            qPrintable( unitStrings[SIZE][GB] ),
                            qPrintable( unitStrings[SIZE][TB] ) );

    mem_K = 1024;
    unitStrings[MEM][B]  = tr(   "B" );
    unitStrings[MEM][KB] = tr( "KiB" );
    unitStrings[MEM][MB] = tr( "MiB" );
    unitStrings[MEM][GB] = tr( "GiB" );
    unitStrings[MEM][TB] = tr( "TiB" );
    tr_formatter_mem_init( mem_K,
                           qPrintable( unitStrings[MEM][KB] ),
                           qPrintable( unitStrings[MEM][MB] ),
                           qPrintable( unitStrings[MEM][GB] ),
                           qPrintable( unitStrings[MEM][TB] ) );
}

/***
****
***/

double
Speed :: KBps( ) const
{
    return _Bps / (double)speed_K;
}

Speed
Speed :: fromKBps( double KBps )
{
    return int( KBps * speed_K );
}

/***
****
***/

QString
Formatter :: memToString( int64_t bytes )
{
    if( bytes < 1 )
        return tr( "Unknown" );
    else if( !bytes )
        return tr( "None" );
    else {
        char buf[128];
        tr_formatter_mem_B( buf, bytes, sizeof( buf ) );
        return buf;
    }
}

QString
Formatter :: sizeToString( int64_t bytes )
{
    if( bytes < 1 )
        return tr( "Unknown" );
    else if( !bytes )
        return tr( "None" );
    else {
        char buf[128];
        tr_formatter_size_B( buf, bytes, sizeof( buf ) );
        return buf;
    }
}

QString
Formatter :: speedToString( const Speed& speed )
{
    if( speed.isZero( ) )
        return tr( "None" );
    else {
        char buf[128];
        tr_formatter_speed_KBps( buf, speed.KBps( ), sizeof( buf ) );
        return buf;
    }
}

QString
Formatter :: percentToString( double x )
{
    char buf[128];
    return QString( tr_strpercent( buf, x, sizeof(buf) ) );
}

QString
Formatter :: ratioToString( double ratio )
{
    char buf[128];
    return QString::fromUtf8( tr_strratio( buf, sizeof(buf), ratio, "\xE2\x88\x9E" ) );
}

QString
Formatter :: timeToString( int seconds )
{
    int days, hours, minutes;
    QString d, h, m, s;
    QString str;

    if( seconds < 0 )
        seconds = 0;

    days = seconds / 86400;
    hours = ( seconds % 86400 ) / 3600;
    minutes = ( seconds % 3600 ) / 60;
    seconds %= 60;

    d = tr( "%Ln day(s)", 0, days );
    h = tr( "%Ln hour(s)", 0, hours );
    m = tr( "%Ln minute(s)", 0, minutes );
    s = tr( "%Ln second(s)", 0, seconds );

    if( days )
    {
        if( days >= 4 || !hours )
            str = d;
        else
            str = tr( "%1, %2" ).arg( d ).arg( h );
    }
    else if( hours )
    {
        if( hours >= 4 || !minutes )
            str = h;
        else
            str = tr( "%1, %2" ).arg( h ).arg( m );
    }
    else if( minutes )
    {
        if( minutes >= 4 || !seconds )
            str = m;
        else
            str = tr( "%1, %2" ).arg( m ).arg( s );
    }
    else
    {
        str = s;
    }

    return str;
}
