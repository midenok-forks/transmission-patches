/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 * $Id: file-tree.cc 13395 2012-07-22 17:01:52Z jordan $
 */

#include <cassert>
#include <iostream>

#include <QApplication>
#include <QHeaderView>
#include <QPainter>
#include <QResizeEvent>
#include <QSortFilterProxyModel>
#include <QStringList>

#include <libtransmission/transmission.h> // priorities

#include "file-tree.h"
#include "formatter.h"
#include "hig.h"
#include "torrent.h" // FileList
#include "utils.h" // mime icons

enum
{
    COL_NAME,
    COL_PROGRESS,
    COL_WANTED,
    COL_PRIORITY,
    NUM_COLUMNS
};

/****
*****
****/

QHash<QString,int>&
FileTreeItem :: getMyChildRows()
{
    const size_t n = childCount( );

    // ensure that all the rows are hashed
    while( myFirstUnhashedRow < n )
    {
      myChildRows.insert (myChildren[myFirstUnhashedRow]->name(), myFirstUnhashedRow);
      myFirstUnhashedRow++;
    }

    return myChildRows;
}


FileTreeItem :: ~FileTreeItem( )
{
    assert( myChildren.isEmpty( ) );

    if( myParent != 0 )
    {
        const int pos = row( );
        assert ((pos>=0) && "couldn't find child in parent's lookup");
        myParent->myChildren.removeAt( pos );
        myParent->myChildRows.remove( name() );
        myParent->myFirstUnhashedRow = pos;
    }
}

void
FileTreeItem :: appendChild( FileTreeItem * child )
{
    const size_t n = childCount();
    child->myParent = this;
    myChildren.append (child);
    myFirstUnhashedRow = n;
}

FileTreeItem *
FileTreeItem :: child (const QString& filename )
{
  FileTreeItem * item(0);

  const int row = getMyChildRows().value (filename, -1);
  if (row != -1)
    {
      item = child (row);
      assert (filename == item->name());
    }

  return item;
}

int
FileTreeItem :: row( ) const
{
    int i(-1);

    if( myParent )
    {
        i = myParent->getMyChildRows().value (name(), -1);
        assert (this == myParent->myChildren[i]);
    }

    return i;
}

QVariant
FileTreeItem :: data( int column ) const
{
    QVariant value;

    switch( column ) {
        case COL_NAME: value.setValue( fileSizeName( ) ); break;
        case COL_PROGRESS: value.setValue( progress( ) ); break;
        case COL_WANTED: value.setValue( isSubtreeWanted( ) ); break;
        case COL_PRIORITY: value.setValue( priorityString( ) ); break;
    }

    return value;
}

void
FileTreeItem :: getSubtreeSize( uint64_t& have, uint64_t& total ) const
{
    have += myHaveSize;
    total += myTotalSize;

    foreach( const FileTreeItem * i, myChildren )
        i->getSubtreeSize( have, total );
}

double
FileTreeItem :: progress( ) const
{
    double d(0);
    uint64_t have(0), total(0);
    getSubtreeSize( have, total );
    if( total )
        d = have / (double)total;
    return d;
}

QString
FileTreeItem :: fileSizeName( ) const
{
    uint64_t have(0), total(0);
    QString str;
    getSubtreeSize( have, total );
    str = QString( name() + " (%1)" ).arg( Formatter::sizeToString( total ) );
    return str;
}

bool
FileTreeItem :: update( int index, bool wanted, int priority, uint64_t totalSize, uint64_t haveSize, bool torrentChanged )
{
    bool changed = false;

    if( myIndex != index )
    {
        myIndex = index;
        changed = true;
    }
    if( torrentChanged && myIsWanted != wanted )
    {
        myIsWanted = wanted;
        changed = true;
    }
    if( torrentChanged && myPriority != priority )
    {
        myPriority = priority;
        changed = true;
    }
    if( myTotalSize != totalSize )
    {
        myTotalSize = totalSize;
        changed = true;
    }
    if( myHaveSize != haveSize )
    {
        myHaveSize = haveSize;
        changed = true;
    }

    return changed;
}

QString
FileTreeItem :: priorityString( ) const
{
    const int i( priority( ) );
    if( i == LOW ) return tr( "Low" );
    if( i == HIGH ) return tr( "High" );
    if( i == NORMAL ) return tr( "Normal" );
    return tr( "Mixed" );
}

int
FileTreeItem :: priority( ) const
{
    int i( 0 );

    if( myChildren.isEmpty( ) ) switch( myPriority ) {
        case TR_PRI_LOW:  i |= LOW; break;
        case TR_PRI_HIGH: i |= HIGH; break;
        default:          i |= NORMAL; break;
    }

    foreach( const FileTreeItem * child, myChildren )
        i |= child->priority( );

    return i;
}

void
FileTreeItem :: setSubtreePriority( int i, QSet<int>& ids )
{
    if( myPriority != i ) {
        myPriority = i;
        if( myIndex >= 0 )
            ids.insert( myIndex );
    }

    foreach( FileTreeItem * child, myChildren )
        child->setSubtreePriority( i, ids );
}

void
FileTreeItem :: twiddlePriority( QSet<int>& ids, int& p )
{
    const int old( priority( ) );

    if     ( old & LOW )    p = TR_PRI_NORMAL;
    else if( old & NORMAL ) p = TR_PRI_HIGH;
    else                    p = TR_PRI_LOW;

    setSubtreePriority( p, ids );
}

int
FileTreeItem :: isSubtreeWanted( ) const
{
    if( myChildren.isEmpty( ) )
        return myIsWanted ? Qt::Checked : Qt::Unchecked;

    int wanted( -1 );
    foreach( const FileTreeItem * child, myChildren ) {
        const int childWanted = child->isSubtreeWanted( );
        if( wanted == -1 )
            wanted = childWanted;
        if( wanted != childWanted )
            wanted = Qt::PartiallyChecked;
        if( wanted == Qt::PartiallyChecked )
            return wanted;
    }

    return wanted;
}

void
FileTreeItem :: setSubtreeWanted( bool b, QSet<int>& ids )
{
    if( myIsWanted != b ) {
        myIsWanted = b;
        if( myIndex >= 0 )
            ids.insert( myIndex );
    }

    foreach( FileTreeItem * child, myChildren )
        child->setSubtreeWanted( b, ids );
}

void
FileTreeItem :: twiddleWanted( QSet<int>& ids, bool& wanted )
{
    wanted = isSubtreeWanted( ) != Qt::Checked;
    setSubtreeWanted( wanted, ids );
}

/***
****
****
***/

FileTreeModel :: FileTreeModel( QObject *parent ):
    QAbstractItemModel(parent)
{
    rootItem = new FileTreeItem( -1 );
}

FileTreeModel :: ~FileTreeModel( )
{
    clear( );

    delete rootItem;
}

QVariant
FileTreeModel :: data( const QModelIndex &index, int role ) const
{
    QVariant value;

    if( index.isValid() && role==Qt::DisplayRole )
    {
        FileTreeItem *item = static_cast<FileTreeItem*>(index.internalPointer());
        value = item->data( index.column( ) );
    }

    return value;
}

Qt::ItemFlags
FileTreeModel :: flags( const QModelIndex& index ) const
{
    int i( Qt::ItemIsSelectable | Qt::ItemIsEnabled );

    if( index.column( ) == COL_WANTED )
        i |= Qt::ItemIsUserCheckable | Qt::ItemIsTristate;

    return (Qt::ItemFlags)i;
}

QVariant
FileTreeModel :: headerData( int column, Qt::Orientation orientation, int role ) const
{
    QVariant data;

    if( orientation==Qt::Horizontal && role==Qt::DisplayRole ) {
        switch( column ) {
            case COL_NAME:     data.setValue( tr( "File" ) ); break;
            case COL_PROGRESS: data.setValue( tr( "Progress" ) ); break;
            case COL_WANTED:   data.setValue( tr( "Download" ) ); break;
            case COL_PRIORITY: data.setValue( tr( "Priority" ) ); break;
            default: break;
        }
    }

    return data;
}

QModelIndex
FileTreeModel :: index( int row, int column, const QModelIndex& parent ) const
{
    QModelIndex i;

    if( !hasIndex( row, column, parent ) )
    {
        std::cerr << " I don't have this index " << std::endl;
    }
    else
    {
        FileTreeItem * parentItem;

        if( !parent.isValid( ) )
            parentItem = rootItem;
        else
            parentItem = static_cast<FileTreeItem*>(parent.internalPointer());

        FileTreeItem * childItem = parentItem->child( row );

        if( childItem )
            i = createIndex( row, column, childItem );

//std::cerr << "FileTreeModel::index(row("<<row<<"),col("<<column<<"),parent("<<qPrintable(parentItem->name())<<")) is returning " << qPrintable(childItem->name()) << ": internalPointer " << i.internalPointer() << " row " << i.row() << " col " << i.column() << std::endl;
    }

    return i;
}

QModelIndex
FileTreeModel :: parent( const QModelIndex& child ) const
{
    return parent( child, 0 ); // QAbstractItemModel::parent() wants col 0
}

QModelIndex
FileTreeModel :: parent( const QModelIndex& child, int column ) const
{
    if( !child.isValid( ) )
        return QModelIndex( );

    FileTreeItem * childItem = static_cast<FileTreeItem*>(child.internalPointer());

    return indexOf( childItem->parent( ), column );
}

int
FileTreeModel :: rowCount( const QModelIndex& parent ) const
{
    FileTreeItem * parentItem;

    if( !parent.isValid( ) )
        parentItem = rootItem;
    else
        parentItem = static_cast<FileTreeItem*>(parent.internalPointer());

    return parentItem->childCount();
}

int
FileTreeModel :: columnCount( const QModelIndex &parent ) const
{
    Q_UNUSED( parent );

    return 4;
}

QModelIndex
FileTreeModel :: indexOf( FileTreeItem * item, int column ) const
{
    if( !item || item==rootItem )
        return QModelIndex( );

    return createIndex( item->row( ), column, item );
}

void
FileTreeModel :: clearSubtree( const QModelIndex& top )
{
    size_t i = rowCount( top );

    while( i > 0 )
        clearSubtree( index( --i, 0, top ) );

    delete static_cast<FileTreeItem*>(top.internalPointer());
}

void
FileTreeModel :: clear( )
{
    clearSubtree( QModelIndex( ) );

    reset( );
}

void
FileTreeModel :: addFile( int                   index,
                          const QString       & filename,
                          bool                  wanted,
                          int                   priority,
                          uint64_t              size,
                          uint64_t              have,
                          QList<QModelIndex>  & rowsAdded,
                          bool                  torrentChanged )
{
    FileTreeItem * i( rootItem );

    foreach( QString token, filename.split( QChar::fromAscii('/') ) )
    {
        FileTreeItem * child( i->child( token ) );
        if( !child )
        {
            QModelIndex parentIndex( indexOf( i, 0 ) );
            const int n( i->childCount( ) );
            beginInsertRows( parentIndex, n, n );
            i->appendChild(( child = new FileTreeItem( -1, token )));
            endInsertRows( );
            rowsAdded.append( indexOf( child, 0 ) );
        }
        i = child;
    }

    if( i != rootItem )
        if( i->update( index, wanted, priority, size, have, torrentChanged ) )
            dataChanged( indexOf( i, 0 ), indexOf( i, NUM_COLUMNS-1 ) );
}

void
FileTreeModel :: parentsChanged( const QModelIndex& index, int column )
{
    QModelIndex walk = index;

    for( ;; ) {
        walk = parent( walk, column );
        if( !walk.isValid( ) )
            break;
        dataChanged( walk, walk );
    }
}

void
FileTreeModel :: subtreeChanged( const QModelIndex& index, int column )
{
    const int childCount = rowCount( index );
    if( !childCount )
        return;

    // tell everyone that this tier changed
    dataChanged( index.child(0,column), index.child(childCount-1,column) );

    // walk the subtiers
    for( int i=0; i<childCount; ++i )
        subtreeChanged( index.child(i,column), column );
}

void
FileTreeModel :: clicked( const QModelIndex& index )
{
    const int column( index.column( ) );

    if( !index.isValid( ) )
        return;

    if( column == COL_WANTED )
    {
        FileTreeItem * item( static_cast<FileTreeItem*>(index.internalPointer()));
        bool want;
        QSet<int> fileIds;
        item->twiddleWanted( fileIds, want );
        emit wantedChanged( fileIds, want );

        dataChanged( index, index );
        parentsChanged( index, column );
        subtreeChanged( index, column );
    }
    else if( column == COL_PRIORITY )
    {
        FileTreeItem * item( static_cast<FileTreeItem*>(index.internalPointer()));
        int priority;
        QSet<int>fileIds;
        item->twiddlePriority( fileIds, priority );
        emit priorityChanged( fileIds, priority );

        dataChanged( index, index );
        parentsChanged( index, column );
        subtreeChanged( index, column );
    }
}

/****
*****
****/

QSize
FileTreeDelegate :: sizeHint( const QStyleOptionViewItem& item, const QModelIndex& index ) const
{
    QSize size;

    switch( index.column( ) )
    {
        case COL_NAME: {
            const QFontMetrics fm( item.font );
            const QString text = index.data().toString();
            const int iconSize = QApplication::style()->pixelMetric( QStyle::PM_SmallIconSize );
            size.rwidth() = HIG::PAD_SMALL + iconSize;
            size.rheight() = std::max( iconSize, fm.height( ) );
            break;
        }

        case COL_PROGRESS:
        case COL_WANTED:
            size = QSize( 20, 1 );
            break;

        default: {
            const QFontMetrics fm( item.font );
            const QString text = index.data().toString();
            size = fm.size( 0, text );
            break;
        }
    }

    size.rheight() += 8; // make the spacing a little nicer
    return size;
}

void
FileTreeDelegate :: paint( QPainter                    * painter,
                           const QStyleOptionViewItem  & option,
                           const QModelIndex           & index ) const
{
    const int column( index.column( ) );

    if( ( column != COL_PROGRESS ) && ( column != COL_WANTED ) && ( column != COL_NAME ) )
    {
        QItemDelegate::paint(painter, option, index);
        return;
    }

    QStyle * style( QApplication :: style( ) );
    if( option.state & QStyle::State_Selected )
        painter->fillRect( option.rect, option.palette.highlight( ) );
    painter->save();
    if( option.state & QStyle::State_Selected )
         painter->setBrush(option.palette.highlightedText());

    if( column == COL_NAME )
    {
        // draw the file icon
        static const int iconSize( style->pixelMetric( QStyle :: PM_SmallIconSize ) );
        const QRect iconArea( option.rect.x(),
                              option.rect.y() + (option.rect.height()-iconSize)/2,
                              iconSize, iconSize );
        QIcon icon;
        if( index.model()->hasChildren( index ) )
            icon = style->standardIcon( QStyle::StandardPixmap( QStyle::SP_DirOpenIcon ) );
        else
        {
            QString name = index.data().toString();
            icon = Utils :: guessMimeIcon( name.left( name.lastIndexOf( " (" ) ) );
        }
        icon.paint( painter, iconArea, Qt::AlignCenter, QIcon::Normal, QIcon::On );

        // draw the name
        QStyleOptionViewItem tmp( option );
        tmp.rect.setWidth( option.rect.width( ) - iconArea.width( ) - HIG::PAD_SMALL );
        tmp.rect.moveRight( option.rect.right( ) );
        QItemDelegate::paint( painter, tmp, index );
    }
    else if( column == COL_PROGRESS )
    {
        QStyleOptionProgressBar p;
        p.state = option.state | QStyle::State_Small;
        p.direction = QApplication::layoutDirection();
        p.rect = option.rect;
        p.rect.setSize( QSize( option.rect.width()-2, option.rect.height()-8 ) );
        p.rect.moveCenter( option.rect.center( ) );
        p.fontMetrics = QApplication::fontMetrics();
        p.minimum = 0;
        p.maximum = 100;
        p.textAlignment = Qt::AlignCenter;
        p.textVisible = true;
        p.progress = (int)(100.0*index.data().toDouble());
        p.text = QString( ).sprintf( "%d%%", p.progress );
        style->drawControl( QStyle::CE_ProgressBar, &p, painter );
    }
    else if( column == COL_WANTED )
    {
        QStyleOptionButton o;
        o.state = option.state;
        o.direction = QApplication::layoutDirection();
        o.rect.setSize( QSize( 20, option.rect.height( ) ) );
        o.rect.moveCenter( option.rect.center( ) );
        o.fontMetrics = QApplication::fontMetrics();
        switch( index.data().toInt() ) {
            case Qt::Unchecked: o.state |= QStyle::State_Off; break;
            case Qt::Checked:   o.state |= QStyle::State_On; break;
            default:            o.state |= QStyle::State_NoChange;break;
        }
        style->drawControl( QStyle::CE_CheckBox, &o, painter );
    }

    painter->restore( );
}

/****
*****
*****
*****
****/

FileTreeView :: FileTreeView( QWidget * parent ):
    QTreeView( parent ),
    myModel( this ),
    myProxy( new QSortFilterProxyModel( ) ),
    myDelegate( this )
{
    setSortingEnabled( true );
    setAlternatingRowColors( true );
    setSelectionBehavior( QAbstractItemView::SelectRows );
    setSelectionMode( QAbstractItemView::ExtendedSelection );
    myProxy->setSourceModel( &myModel );
    setModel( myProxy );
    setItemDelegate( &myDelegate );
    setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    sortByColumn( COL_NAME, Qt::AscendingOrder );
    installEventFilter( this );

    for( int i=0; i<NUM_COLUMNS; ++i )
        header()->setResizeMode( i, QHeaderView::Interactive );

    connect( this, SIGNAL(clicked(const QModelIndex&)),
             this, SLOT(onClicked(const QModelIndex&)) );

    connect( &myModel, SIGNAL(priorityChanged(const QSet<int>&, int)),
             this,     SIGNAL(priorityChanged(const QSet<int>&, int)));

    connect( &myModel, SIGNAL(wantedChanged(const QSet<int>&, bool)),
             this,     SIGNAL(wantedChanged(const QSet<int>&, bool)));
}

FileTreeView :: ~FileTreeView( )
{
    myProxy->deleteLater();
}

void
FileTreeView :: onClicked( const QModelIndex& proxyIndex )
{
    const QModelIndex modelIndex = myProxy->mapToSource( proxyIndex );
    myModel.clicked( modelIndex );
}

bool
FileTreeView :: eventFilter( QObject * o, QEvent * event )
{
    if( o != this )
        return false;

    // this is kind of a hack to get the last three columns be the
    // right size, and to have the filename column use whatever
    // space is left over...
    if( event->type() == QEvent::Resize )
    {
        QResizeEvent * r = dynamic_cast<QResizeEvent*>(event);
        int left = r->size().width();
        const QFontMetrics fontMetrics( font( ) );
        for( int column=0; column<NUM_COLUMNS; ++column ) {
            if( column == COL_NAME )
                continue;
            if( isColumnHidden( column ) )
                continue;
            const QString header = myModel.headerData( column, Qt::Horizontal ).toString( ) + "    ";
            const int width = fontMetrics.size( 0, header ).width( );
            setColumnWidth( column, width );
            left -= width;
        }
        left -= 20; // not sure why this is necessary.  it works in different themes + font sizes though...
        setColumnWidth( COL_NAME, std::max(left,0) );
        return false;
    }

    // handle using the keyboard to toggle the
    // wanted/unwanted state or the file priority
    else if( event->type() == QEvent::KeyPress )
    {
        switch( dynamic_cast<QKeyEvent*>(event)->key() )
        {
            case Qt::Key_Space:
                foreach( QModelIndex i, selectionModel()->selectedRows(COL_WANTED) )
                    clicked( i );
                return false;

            case Qt::Key_Enter:
            case Qt::Key_Return:
                foreach( QModelIndex i, selectionModel()->selectedRows(COL_PRIORITY) )
                    clicked( i );
                return false;
        }
    }

    return false;
}

void
FileTreeView :: update( const FileList& files )
{
    update( files, true );
}

void
FileTreeView :: update( const FileList& files, bool torrentChanged )
{
    foreach( const TrFile file, files ) {
        QList<QModelIndex> added;
        myModel.addFile( file.index, file.filename, file.wanted, file.priority, file.size, file.have, added, torrentChanged );
        foreach( QModelIndex i, added )
            expand( myProxy->mapFromSource( i ) );
    }
}

void
FileTreeView :: clear( )
{
    myModel.clear( );
}
