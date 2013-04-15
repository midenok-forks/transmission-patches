/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 * $Id: utils.h 13448 2012-08-19 16:12:20Z jordan $
 */

#ifndef QTR_UTILS
#define QTR_UTILS

#include <QString>
#include <QObject>
#include <QIcon>

#include <cctype> // isxdigit()

#include "speed.h"

class Utils: public QObject
{
        Q_OBJECT

    public:
        Utils( ) { }
        virtual ~Utils( ) { }

    public:
        static QString remoteFileChooser( QWidget * parent, const QString& title, const QString& myPath, bool dir, bool local );
        static const QIcon& guessMimeIcon( const QString& filename );
        // Test if string is UTF-8 or not
        static bool isValidUtf8 ( const char *s );

        // meh
        static void toStderr( const QString& qstr );

        ///
        /// URLs
        ///

        static bool isMagnetLink( const QString& s ) { return s.startsWith( QString::fromAscii( "magnet:?" ) ); }

        static bool isHexHashcode( const QString& s )
        {
            if( s.length() != 40 ) return false;
            foreach( QChar ch, s ) if( !isxdigit( ch.toAscii() ) ) return false;
            return true;
        }

        static bool isUriWithSupportedScheme( const QString& s )
        {
            static const QString ftp = QString::fromAscii( "ftp://" );
            static const QString http = QString::fromAscii( "http://" );
            static const QString https = QString::fromAscii( "https://" );
            return s.startsWith(http) || s.startsWith(https) || s.startsWith(ftp);
        }

};

#endif
