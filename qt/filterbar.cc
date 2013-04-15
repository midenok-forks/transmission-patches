/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 * $Id: filterbar.cc 12607 2011-08-01 22:24:24Z jordan $
 */

#include <QString>
#include <QtGui>

#include "app.h"
#include "favicon.h"
#include "filters.h"
#include "filterbar.h"
#include "hig.h"
#include "prefs.h"
#include "torrent-filter.h"
#include "torrent-model.h"
#include "utils.h"

/****
*****
*****  DELEGATE
*****
****/

enum
{
    TorrentCountRole = Qt::UserRole + 1,
    ActivityRole,
    TrackerRole
};

namespace
{
    int getHSpacing( QWidget * w )
    {
        return qMax( int(HIG::PAD_SMALL), w->style()->pixelMetric( QStyle::PM_LayoutHorizontalSpacing, 0, w ) );
    }
}

FilterBarComboBoxDelegate :: FilterBarComboBoxDelegate( QObject * parent, QComboBox * combo ):
    QItemDelegate( parent ),
    myCombo( combo )
{
}

bool
FilterBarComboBoxDelegate :: isSeparator( const QModelIndex &index )
{
    return index.data(Qt::AccessibleDescriptionRole).toString() == QLatin1String("separator");
}
void
FilterBarComboBoxDelegate :: setSeparator( QAbstractItemModel * model, const QModelIndex& index )
{
    model->setData( index, QString::fromLatin1("separator"), Qt::AccessibleDescriptionRole );

    if( QStandardItemModel *m = qobject_cast<QStandardItemModel*>(model) )
       if (QStandardItem *item = m->itemFromIndex(index))
           item->setFlags(item->flags() & ~(Qt::ItemIsSelectable|Qt::ItemIsEnabled));
}

void
FilterBarComboBoxDelegate :: paint( QPainter                    * painter,
                                    const QStyleOptionViewItem  & option,
                                    const QModelIndex           & index ) const
{
    if( isSeparator( index ) )
    {
        QRect rect = option.rect;
        if (const QStyleOptionViewItemV3 *v3 = qstyleoption_cast<const QStyleOptionViewItemV3*>(&option))
            if (const QAbstractItemView *view = qobject_cast<const QAbstractItemView*>(v3->widget))
                rect.setWidth(view->viewport()->width());
        QStyleOption opt;
        opt.rect = rect;
        myCombo->style()->drawPrimitive(QStyle::PE_IndicatorToolBarSeparator, &opt, painter, myCombo);
    }
    else
    {
        QStyleOptionViewItem disabledOption = option;
        disabledOption.state &= ~( QStyle::State_Enabled | QStyle::State_Selected );
        QRect boundingBox = option.rect;

        const int hmargin = getHSpacing( myCombo );
        boundingBox.setLeft( boundingBox.left() + hmargin );
        boundingBox.setRight( boundingBox.right() - hmargin );

        QRect decorationRect = rect( option, index, Qt::DecorationRole );
        decorationRect.moveLeft( decorationRect.left( ) );
        decorationRect.setSize( myCombo->iconSize( ) );
        decorationRect = QStyle::alignedRect( Qt::LeftToRight,
                                              Qt::AlignLeft|Qt::AlignVCenter,
                                              decorationRect.size(), boundingBox );
        boundingBox.setLeft( decorationRect.right() + hmargin );

        QRect countRect  = rect( option, index, TorrentCountRole );
        countRect = QStyle::alignedRect( Qt::LeftToRight,
                                         Qt::AlignRight|Qt::AlignVCenter,
                                         countRect.size(), boundingBox );
        boundingBox.setRight( countRect.left() - hmargin );
        const QRect displayRect = boundingBox;

        drawBackground( painter, option, index );
        QStyleOptionViewItem option2 = option;
        option2.decorationSize = myCombo->iconSize( );
        drawDecoration( painter, option, decorationRect, decoration(option2,index.data(Qt::DecorationRole)) );
        drawDisplay( painter, option, displayRect, index.data(Qt::DisplayRole).toString() );
        drawDisplay( painter, disabledOption, countRect, index.data(TorrentCountRole).toString() );
        drawFocus( painter, option, displayRect|countRect );
    }
}

QSize
FilterBarComboBoxDelegate :: sizeHint( const QStyleOptionViewItem & option,
                                       const QModelIndex          & index ) const
{
    if( isSeparator( index ) )
    {
        const int pm = myCombo->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, 0, myCombo);
        return QSize( pm, pm + 10 );
    }
    else
    {
        QStyle * s = myCombo->style( );
        const int hmargin = getHSpacing( myCombo );

        QSize size = QItemDelegate::sizeHint( option, index );
        size.setHeight( qMax( size.height(), myCombo->iconSize().height() + 6 ) );
        size.rwidth() += s->pixelMetric( QStyle::PM_FocusFrameHMargin, 0, myCombo );
        size.rwidth() += rect(option,index,TorrentCountRole).width();
        size.rwidth() += hmargin * 4;
        return size;
    }
}

/**
***
**/

FilterBarComboBox :: FilterBarComboBox( QWidget * parent ):
    QComboBox( parent )
{
}

void
FilterBarComboBox :: paintEvent( QPaintEvent * e )
{
    Q_UNUSED( e );

    QStylePainter painter(this);
    painter.setPen(palette().color(QPalette::Text));

    // draw the combobox frame, focusrect and selected etc.
    QStyleOptionComboBox opt;
    initStyleOption(&opt);
    painter.drawComplexControl(QStyle::CC_ComboBox, opt);

    // draw the icon and text
    const QModelIndex modelIndex = model()->index( currentIndex(), 0, rootModelIndex() );
    if( modelIndex.isValid( ) )
    {
        QStyle * s = style();
        QRect rect = s->subControlRect( QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, this );
        const int hmargin = getHSpacing( this );
        rect.setRight( rect.right() - hmargin );

        // draw the icon
        QPixmap pixmap;
        QVariant variant = modelIndex.data( Qt::DecorationRole );
        switch( variant.type( ) ) {
            case QVariant::Pixmap: pixmap = qvariant_cast<QPixmap>(variant); break;
            case QVariant::Icon:   pixmap = qvariant_cast<QIcon>(variant).pixmap(iconSize()); break;
            default: break;
        }
        if( !pixmap.isNull() ) {
            s->drawItemPixmap( &painter, rect, Qt::AlignLeft|Qt::AlignVCenter, pixmap );
            rect.setLeft( rect.left() + pixmap.width() + hmargin );
        }

        // draw the count
        QString text = modelIndex.data(TorrentCountRole).toString();
        if( !text.isEmpty( ) )
        {
            const QPen pen = painter.pen( );
            painter.setPen( opt.palette.color( QPalette::Disabled, QPalette::Text ) );
            QRect r = s->itemTextRect( painter.fontMetrics(), rect, Qt::AlignRight|Qt::AlignVCenter, false, text );
            painter.drawText( r, 0, text );
            rect.setRight( r.left() - hmargin );
            painter.setPen( pen );
        }

        // draw the text
        text = modelIndex.data( Qt::DisplayRole ).toString();
        text = painter.fontMetrics().elidedText ( text, Qt::ElideRight, rect.width() );
        s->drawItemText( &painter, rect, Qt::AlignLeft|Qt::AlignVCenter, opt.palette, true, text );
    }
}

/****
*****
*****  ACTIVITY
*****
****/

QComboBox*
FilterBar :: createActivityCombo( )
{
    QComboBox * c = new FilterBarComboBox( this );
    FilterBarComboBoxDelegate * delegate = new FilterBarComboBoxDelegate( 0, c );
    c->setItemDelegate( delegate );

    QPixmap blankPixmap( c->iconSize( ) );
    blankPixmap.fill( Qt::transparent );
    QIcon blankIcon( blankPixmap );

    QStandardItemModel * model = new QStandardItemModel;

    QStandardItem * row = new QStandardItem( tr( "All" ) );
    row->setData( FilterMode::SHOW_ALL, ActivityRole );
    model->appendRow( row );

    model->appendRow( new QStandardItem ); // separator
    delegate->setSeparator( model, model->index( 1, 0 ) );

    row = new QStandardItem( QIcon::fromTheme( "system-run", blankIcon ), tr( "Active" ) );
    row->setData( FilterMode::SHOW_ACTIVE, ActivityRole );
    model->appendRow( row );

    row = new QStandardItem( QIcon::fromTheme( "go-down", blankIcon ), tr( "Downloading" ) );
    row->setData( FilterMode::SHOW_DOWNLOADING, ActivityRole );
    model->appendRow( row );

    row = new QStandardItem( QIcon::fromTheme( "go-up", blankIcon ), tr( "Seeding" ) );
    row->setData( FilterMode::SHOW_SEEDING, ActivityRole );
    model->appendRow( row );

    row = new QStandardItem( QIcon::fromTheme( "media-playback-pause", blankIcon ), tr( "Paused" ) );
    row->setData( FilterMode::SHOW_PAUSED, ActivityRole );
    model->appendRow( row );

    row = new QStandardItem( blankIcon, tr( "Finished" ) );
    row->setData( FilterMode::SHOW_FINISHED, ActivityRole );
    model->appendRow( row );

    row = new QStandardItem( QIcon::fromTheme( "view-refresh", blankIcon ), tr( "Verifying" ) );
    row->setData( FilterMode::SHOW_VERIFYING, ActivityRole );
    model->appendRow( row );

    row = new QStandardItem( QIcon::fromTheme( "dialog-error", blankIcon ), tr( "Error" ) );
    row->setData( FilterMode::SHOW_ERROR, ActivityRole );
    model->appendRow( row );

    c->setModel( model );
    return c;
}

/****
*****
*****
*****
****/

namespace
{
    QString readableHostName( const QString host )
    {
        // get the readable name...
        QString name = host;
        const int pos = name.lastIndexOf( '.' );
        if( pos >= 0 )
            name.truncate( pos );
        if( !name.isEmpty( ) )
            name[0] = name[0].toUpper( );
        return name;
    }
}

void
FilterBar :: refreshTrackers( )
{
    Favicons& favicons = dynamic_cast<MyApp*>(QApplication::instance())->favicons;
    const int firstTrackerRow = 2; // skip over the "All" and separator...

    // pull info from the tracker model...
    QSet<QString> oldHosts;
    for( int row=firstTrackerRow; ; ++row ) {
        QModelIndex index = myTrackerModel->index( row, 0 );
        if( !index.isValid( ) )
            break;
        oldHosts << index.data(TrackerRole).toString();
    }

    // pull the new stats from the torrent model...
    QSet<QString> newHosts;
    QMap<QString,int> torrentsPerHost;
    for( int row=0; ; ++row )
    {
        QModelIndex index = myTorrents.index( row, 0 );
        if( !index.isValid( ) )
            break;
        const Torrent * tor = index.data( TorrentModel::TorrentRole ).value<const Torrent*>();
        const QStringList trackers = tor->trackers( );
        QSet<QString> torrentNames;
        foreach( QString tracker, trackers ) {
            const QString host = Favicons::getHost( QUrl( tracker ) );
            if( host.isEmpty( ) )
                qWarning() << "torrent" << qPrintable(tor->name()) << "has an invalid announce URL:" << tracker;
            else {
                newHosts.insert( host );
                torrentNames.insert( readableHostName( host ) );
            }
        }
        foreach( QString name, torrentNames )
            ++torrentsPerHost[ name ];
    }

    // update the "All" row
    myTrackerModel->setData( myTrackerModel->index(0,0), getCountString(myTorrents.rowCount()), TorrentCountRole );

    // rows to update
    foreach( QString host, oldHosts & newHosts )
    {
        const QString name = readableHostName( host );
        QStandardItem * row = myTrackerModel->findItems(name).front();
        row->setData( getCountString(torrentsPerHost[name]), TorrentCountRole );
        row->setData( favicons.findFromHost(host), Qt::DecorationRole );
    }

    // rows to remove
    foreach( QString host, oldHosts - newHosts ) {
        const QString name = readableHostName( host );
        QStandardItem * item = myTrackerModel->findItems(name).front();
        if( !item->data(TrackerRole).toString().isEmpty() ) // don't remove "All"
            myTrackerModel->removeRows( item->row(), 1 );
    }

    // rows to add
    bool anyAdded = false;
    foreach( QString host, newHosts - oldHosts )
    {
        const QString name = readableHostName( host );

        if( !myTrackerModel->findItems(name).isEmpty() )
            continue;

        // find the sorted position to add this row
        int i = firstTrackerRow;
        for( int n=myTrackerModel->rowCount(); i<n; ++i ) {
            const QString rowName = myTrackerModel->index(i,0).data(Qt::DisplayRole).toString();
            if( rowName >= name )
                break;
        }

        // add the row
        QStandardItem * row = new QStandardItem( favicons.findFromHost( host ), name );
        row->setData( getCountString(torrentsPerHost[host]), TorrentCountRole );
        row->setData( favicons.findFromHost(host), Qt::DecorationRole );
        row->setData( host, TrackerRole );
        myTrackerModel->insertRow( i, row );
        anyAdded = true;
    }

    if( anyAdded ) // the one added might match our filter...
        refreshPref( Prefs::FILTER_TRACKERS );
}


QComboBox*
FilterBar :: createTrackerCombo( QStandardItemModel * model )
{
    QComboBox * c = new FilterBarComboBox( this );
    FilterBarComboBoxDelegate * delegate = new FilterBarComboBoxDelegate( 0, c );
    c->setItemDelegate( delegate );

    QStandardItem * row = new QStandardItem( tr( "All" ) );
    row->setData( "", TrackerRole );
    row->setData( getCountString(myTorrents.rowCount()), TorrentCountRole );
    model->appendRow( row );

    model->appendRow( new QStandardItem ); // separator
    delegate->setSeparator( model, model->index( 1, 0 ) );

    c->setModel( model );
    return c;
}

/****
*****
*****
*****
****/

FilterBar :: FilterBar( Prefs& prefs, TorrentModel& torrents, TorrentFilter& filter, QWidget * parent ):
    QWidget( parent ),
    myPrefs( prefs ),
    myTorrents( torrents ),
    myFilter( filter ),
    myRecountTimer( new QTimer( this ) ),
    myIsBootstrapping( true )
{
    QHBoxLayout * h = new QHBoxLayout( this );
    const int hmargin = qMax( int(HIG::PAD), style()->pixelMetric( QStyle::PM_LayoutHorizontalSpacing ) );

    h->setSpacing( 0 );
    h->setContentsMargins( 2, 2, 2, 2 );
    h->addWidget( new QLabel( tr( "Show:" ), this ) );
    h->addSpacing( hmargin );

    myActivityCombo = createActivityCombo( );
    h->addWidget( myActivityCombo, 1 );
    h->addSpacing( hmargin );

    myTrackerModel = new QStandardItemModel;
    myTrackerCombo = createTrackerCombo( myTrackerModel );
    h->addWidget( myTrackerCombo, 1 );
    h->addSpacing( hmargin*2 );

    myLineEdit = new QLineEdit( this );
    h->addWidget( myLineEdit );
    connect( myLineEdit, SIGNAL(textChanged(QString)), this, SLOT(onTextChanged(QString)));

    QPushButton * p = new QPushButton;
    QIcon icon = QIcon::fromTheme( "edit-clear", style()->standardIcon( QStyle::SP_DialogCloseButton ) );
    int iconSize = style()->pixelMetric( QStyle::PM_SmallIconSize );
    p->setIconSize( QSize( iconSize, iconSize ) );
    p->setIcon( icon );
    p->setFlat( true );
    h->addWidget( p );
    connect( p, SIGNAL(clicked(bool)), myLineEdit, SLOT(clear()));

    // listen for changes from the other players
    connect( &myPrefs, SIGNAL(changed(int)), this, SLOT(refreshPref(int)));
    connect( myActivityCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(onActivityIndexChanged(int)));
    connect( myTrackerCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(onTrackerIndexChanged(int)));
    connect( &myTorrents, SIGNAL(modelReset()), this, SLOT(onTorrentModelReset()));
    connect( &myTorrents, SIGNAL(rowsInserted(const QModelIndex&,int,int)), this, SLOT(onTorrentModelRowsInserted(const QModelIndex&,int,int)));
    connect( &myTorrents, SIGNAL(rowsRemoved(const QModelIndex&,int,int)), this, SLOT(onTorrentModelRowsRemoved(const QModelIndex&,int,int)));
    connect( &myTorrents, SIGNAL(dataChanged(const QModelIndex&,const QModelIndex&)), this, SLOT(onTorrentModelDataChanged(const QModelIndex&,const QModelIndex&)));
    connect( myRecountTimer, SIGNAL(timeout()), this, SLOT(recount()) );

    recountSoon( );
    refreshTrackers( );
    myIsBootstrapping = false;

    // initialize our state
    QList<int> initKeys;
    initKeys << Prefs :: FILTER_MODE
             << Prefs :: FILTER_TRACKERS;
    foreach( int key, initKeys )
        refreshPref( key );
}

FilterBar :: ~FilterBar( )
{
    delete myRecountTimer;
}

/***
****
***/

void
FilterBar :: refreshPref( int key )
{
    switch( key )
    {
        case Prefs :: FILTER_MODE: {
            const FilterMode m = myPrefs.get<FilterMode>( key );
            QAbstractItemModel * model = myActivityCombo->model( );
            QModelIndexList indices = model->match( model->index(0,0), ActivityRole, m.mode(), -1 );
            myActivityCombo->setCurrentIndex( indices.isEmpty() ? 0 : indices.first().row( ) );
            break;
        }

        case Prefs :: FILTER_TRACKERS: {
            const QString tracker = myPrefs.getString( key );
            const QString name = readableHostName( tracker );
            QList<QStandardItem*> rows = myTrackerModel->findItems(name);
            if( !rows.isEmpty() )
                myTrackerCombo->setCurrentIndex( rows.front()->row() );
            else { // hm, we don't seem to have this tracker anymore...
                const bool isBootstrapping = myTrackerModel->rowCount( ) <= 2;
                if( !isBootstrapping )
                    myPrefs.set( key, "" );
            }
            break;
        }

        case Prefs :: FILTER_TEXT:
            myLineEdit->setText( myPrefs.getString( key ) );
            break;
    }
}

void
FilterBar :: onTextChanged( const QString& str )
{
    if( !myIsBootstrapping )
        myPrefs.set( Prefs::FILTER_TEXT, str.trimmed( ) );
}

void
FilterBar :: onTrackerIndexChanged( int i )
{
    if( !myIsBootstrapping )
    {
        QString str;
        const bool isTracker = !myTrackerCombo->itemData(i,TrackerRole).toString().isEmpty();
        if( !isTracker ) // show all
            str = "";
        else {
            str = myTrackerCombo->itemData(i,TrackerRole).toString();
            const int pos = str.lastIndexOf( '.' );
            if( pos >= 0 )
              str.truncate( pos+1 );
        }
        myPrefs.set( Prefs::FILTER_TRACKERS, str );
    }
}

void
FilterBar :: onActivityIndexChanged( int i )
{
    if( !myIsBootstrapping )
    {
        const FilterMode mode = myActivityCombo->itemData( i, ActivityRole ).toInt( );
        myPrefs.set( Prefs::FILTER_MODE, mode );
    }
}

/***
****
***/

void FilterBar :: onTorrentModelReset( ) { recountSoon( ); }
void FilterBar :: onTorrentModelRowsInserted( const QModelIndex&, int, int ) { recountSoon( ); }
void FilterBar :: onTorrentModelRowsRemoved( const QModelIndex&, int, int ) { recountSoon( ); }
void FilterBar :: onTorrentModelDataChanged( const QModelIndex&, const QModelIndex& ) { recountSoon( ); }

void
FilterBar :: recountSoon( )
{
    if( !myRecountTimer->isActive( ) )
    {
        myRecountTimer->setSingleShot( true );
        myRecountTimer->start( 500 );
    }
}
void
FilterBar :: recount ( )
{
    // recount the activity combobox...
    for( int i=0, n=FilterMode::NUM_MODES; i<n; ++i )
    {
        const FilterMode m( i );
        QAbstractItemModel * model = myActivityCombo->model( );
        QModelIndexList indices = model->match( model->index(0,0), ActivityRole, m.mode(), -1 );
        if( !indices.isEmpty( ) )
            model->setData( indices.first(), getCountString(myFilter.count(m)), TorrentCountRole );
    }

    refreshTrackers( );
}

QString
FilterBar :: getCountString( int n ) const
{
    return QString("%L1").arg(n);
}
