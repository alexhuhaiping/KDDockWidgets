/*
  This file is part of KDDockWidgets.

  Copyright (C) 2020 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Sérgio Martins <sergio.martins@kdab.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Item_p.h"
#include "Separator_p.h"
#include "MultiSplitterConfig.h"
#include "Widget.h"

#include <QEvent>
#include <QDebug>
#include <QScopedValueRollback>
#include <QTimer>
#include <QGuiApplication>
#include <QScreen>

#ifdef Q_CC_MSVC
# pragma warning(push)
# pragma warning(disable:4138)
# pragma warning(disable:4244)
# pragma warning(disable:4457)
# pragma warning(disable:4702)
#endif

using namespace Layouting;

int Layouting::Item::separatorThickness = 5;
const QSize Layouting::Item::hardcodedMinimumSize = QSize(KDDOCKWIDGETS_MIN_WIDTH, KDDOCKWIDGETS_MIN_HEIGHT);
const QSize Layouting::Item::hardcodedMaximumSize = QSize(KDDOCKWIDGETS_MAX_WIDTH, KDDOCKWIDGETS_MAX_HEIGHT);

bool Layouting::ItemContainer::s_inhibitSimplify = false;

inline bool locationIsVertical(Item::Location loc)
{
    return loc == Item::Location_OnTop || loc == Item::Location_OnBottom;
}

inline bool locationIsSide1(Item::Location loc)
{
    return loc == Item::Location_OnLeft || loc == Item::Location_OnTop;
}

inline Qt::Orientation orientationForLocation(Item::Location loc)
{
    switch (loc) {
    case Item::Location_OnLeft:
    case Item::Location_OnRight:
        return Qt::Horizontal;
    case Item::Location_None:
    case Item::Location_OnTop:
    case Item::Location_OnBottom:
        return Qt::Vertical;
    }

    return Qt::Vertical;
}

inline Qt::Orientation oppositeOrientation(Qt::Orientation o)
{
    return o == Qt::Vertical ? Qt::Horizontal
                             : Qt::Vertical;
}

inline QRect adjustedRect(QRect r, Qt::Orientation o, int p1, int p2)
{
    if (o == Qt::Vertical) {
        r.adjust(0, p1, 0, p2);
    } else {
        r.adjust(p1, 0, p2, 0);
    }

    return r;
}

namespace Layouting {
struct LengthOnSide
{
    int length = 0;
    int minLength = 0;

    int available() const {
        return qMax(0, length - minLength);
    }

    int missing() const {
        return qMax(0, minLength - length);
    }
};
}

ItemContainer *Item::root() const
{
    return m_parent ? m_parent->root()
                    : const_cast<ItemContainer*>(qobject_cast<const ItemContainer*>(this));
}

QRect Item::mapToRoot(QRect r) const
{
    const QPoint topLeft = mapToRoot(r.topLeft());
    r.moveTopLeft(topLeft);
    return r;
}

QPoint Item::mapToRoot(QPoint p) const
{
    if (isRoot())
        return p;

    return p + parentContainer()->mapToRoot(pos());
}

int Item::mapToRoot(int p, Qt::Orientation o) const
{
    if (o == Qt::Vertical)
        return mapToRoot(QPoint(0, p)).y();
    return mapToRoot(QPoint(p, 0)).x();
}

QPoint Item::mapFromRoot(QPoint p) const
{
    const Item *it = this;
    while (it) {
        p = p - it->pos();
        it = it->parentContainer();
    }

    return p;
}

QRect Item::mapFromRoot(QRect r) const
{
    const QPoint topLeft = mapFromRoot(r.topLeft());
    r.moveTopLeft(topLeft);
    return r;
}

QPoint Item::mapFromParent(QPoint p) const
{
    if (isRoot())
        return p;

    return p - pos();
}

int Item::mapFromRoot(int p, Qt::Orientation o) const
{
    if (o == Qt::Vertical)
        return mapFromRoot(QPoint(0, p)).y();
    return mapFromRoot(QPoint(p, 0)).x();
}

QObject *Item::guestAsQObject() const
{
    return m_guest ? m_guest->asQObject() : nullptr;
}

void Item::setGuestWidget(Widget *guest)
{   
    Q_ASSERT(!guest || !m_guest);
    QObject *newWidget = guest ? guest->asQObject() : nullptr;
    QObject *oldWidget = guestAsQObject();

    if (oldWidget) {
        oldWidget->removeEventFilter(this);
        disconnect(oldWidget, nullptr, this, nullptr);
    }

    m_guest = guest;

    if (m_guest) {
        m_guest->setLayoutItem(this);
        newWidget->installEventFilter(this);
        m_guest->setParent(m_hostWidget);
        setMinSize(guest->minSize());
        setMaxSizeHint(guest->maxSizeHint());

        connect(newWidget, &QObject::objectNameChanged, this, &Item::updateObjectName);
        connect(newWidget, &QObject::destroyed, this, &Item::onWidgetDestroyed);
        connect(newWidget, SIGNAL(layoutInvalidated()), this, SLOT(onWidgetLayoutRequested()));

        if (m_sizingInfo.geometry.isEmpty()) {
            // Use the widgets geometry, but ensure it's at least hardcodedMinimumSize
            QRect widgetGeo = m_guest->geometry();
            widgetGeo.setSize(widgetGeo.size().expandedTo(Item::hardcodedMinimumSize));
            setGeometry(mapFromRoot(widgetGeo));
        } else {
            updateWidgetGeometries();
        }
    }

    updateObjectName();
}

void Item::updateWidgetGeometries()
{
    if (m_guest) {
        m_guest->setGeometry(mapToRoot(rect()));
    }
}

QVariantMap Item::toVariantMap() const
{
    QVariantMap result;

    result[QStringLiteral("sizingInfo")] = m_sizingInfo.toVariantMap();
    result[QStringLiteral("isVisible")] = m_isVisible;
    result[QStringLiteral("isContainer")] = isContainer();
    result[QStringLiteral("objectName")] = objectName();
    if (m_guest)
        result[QStringLiteral("guestId")] = m_guest->id(); // just for coorelation purposes when restoring

    return result;
}

void Item::fillFromVariantMap(const QVariantMap &map, const QHash<QString, Widget *> &widgets)
{
    m_sizingInfo.fromVariantMap(map[QStringLiteral("sizingInfo")].toMap());
    m_isVisible = map[QStringLiteral("isVisible")].toBool();
    setObjectName(map[QStringLiteral("objectName")].toString());

    const QString guestId = map.value(QStringLiteral("guestId")).toString();
    if (!guestId.isEmpty()) {
        if (Widget *guest = widgets.value(guestId)) {
            setGuestWidget(guest);
            m_guest->setParent(hostWidget());
        } else if (hostWidget()) {
            qWarning() << Q_FUNC_INFO << "Couldn't find frame to restore for" << this;
        }
    }
}

Item *Item::createFromVariantMap(Widget *hostWidget, ItemContainer *parent,
                                 const QVariantMap &map, const QHash<QString, Widget *> &widgets)
{
    auto item = new Item(hostWidget, parent);
    item->fillFromVariantMap(map, widgets);
    return item;
}

void Item::ref()
{
    m_refCount++;
}

void Item::unref()
{
    Q_ASSERT(m_refCount > 0);
    m_refCount--;
    if (m_refCount == 0) {
        Q_ASSERT(!isRoot());
        parentContainer()->removeItem(this);
    }
}

int Item::refCount() const
{
    return m_refCount;
}

Widget *Item::hostWidget() const
{
    return m_hostWidget;
}

QObject *Item::host() const
{
    return m_hostWidget ? m_hostWidget->asQObject()
                        : nullptr;
}

void Item::restore(Widget *guest)
{
    Q_ASSERT(!isVisible() && !guestAsQObject());
    if (isContainer()) {
        qWarning() << Q_FUNC_INFO << "Containers can't be restored";
    } else {
        setGuestWidget(guest);
        parentContainer()->restoreChild(this, NeighbourSqueezeStrategy::ImmediateNeighboursFirst);

        // When we restore to previous positions, we only still from the immediate neighbours.
        // It's consistent with closing an item, it also only grows the immediate neighbours
        // By passing ImmediateNeighboursFirst we can hide/show an item multiple times and it
        // uses the same place
    }
}

QVector<int> Item::pathFromRoot() const
{
    // Returns the list of indexes to get to this item, starting from the root container
    // Example [0, 1, 3] would mean that the item is the 4th child of the 2nd child of the 1st child of root
    // [] would mean 'this' is the root item
    // [0] would mean the 1st child of root

    QVector<int> path;
    path.reserve(10); // random big number, good to bootstrap it

    const Item *it = this;
    while (it) {
        if (auto p = it->parentContainer()) {
            const int index = p->childItems().indexOf(const_cast<Item*>(it));
            path.prepend(index);
            it = p;
        } else {
            break;
        }
    }

    return path;
}

void Item::setHostWidget(Widget *host)
{
    if (m_hostWidget != host) {
        m_hostWidget = host;
        if (m_guest) {
            m_guest->setParent(host);
            m_guest->setVisible(true);
            updateWidgetGeometries();
        }
    }
}

void Item::setSize_recursive(QSize newSize, ChildrenResizeStrategy)
{
    setSize(newSize);
}

QSize Item::missingSize() const
{
    QSize missing = minSize() - this->size();
    missing.setWidth(qMax(missing.width(), 0));
    missing.setHeight(qMax(missing.height(), 0));

    return missing;
}

bool Item::isBeingInserted() const
{
    return m_sizingInfo.isBeingInserted;
}

void Item::setBeingInserted(bool is)
{
    m_sizingInfo.isBeingInserted = is;

    // Trickle up the hierarchy too, as the parent might be hidden due to not having visible children
    if (auto parent = parentContainer()) {
        if (is) {
            if (!parent->hasVisibleChildren())
                parent->setBeingInserted(true);
        } else {
            parent->setBeingInserted(false);
        }
    }
}

void Item::setParentContainer(ItemContainer *parent)
{
    if (parent == m_parent)
        return;

    if (m_parent) {
        disconnect(this, &Item::minSizeChanged, m_parent, &ItemContainer::onChildMinSizeChanged);
        disconnect(this, &Item::visibleChanged, m_parent, &ItemContainer::onChildVisibleChanged);
        Q_EMIT visibleChanged(this, false);
    }

    if (auto c = asContainer()) {
        const bool ceasingToBeRoot = !m_parent && parent;
        if (ceasingToBeRoot && !c->hasVisibleChildren()) {
            // Was root but is not root anymore. So, if empty, then it has an empty rect too.
            // Only root can have a non-empty rect without having children
            c->setGeometry({});
        }
    }

    m_parent = parent;
    connectParent(parent); // Reused by the ctor too

    QObject::setParent(parent);
}

void Item::connectParent(ItemContainer *parent)
{
    if (parent) {
        connect(this, &Item::minSizeChanged, parent, &ItemContainer::onChildMinSizeChanged);
        connect(this, &Item::visibleChanged, parent, &ItemContainer::onChildVisibleChanged);

        setHostWidget(parent->hostWidget());
        updateWidgetGeometries();

        Q_EMIT visibleChanged(this, isVisible());
    }
}

ItemContainer *Item::parentContainer() const
{
    return m_parent;
}

const ItemContainer *Item::asContainer() const
{
    return qobject_cast<const ItemContainer*>(this);
}

ItemContainer *Item::asContainer()
{
    return qobject_cast<ItemContainer*>(this);
}

void Item::setMinSize(QSize sz)
{
    if (sz != m_sizingInfo.minSize) {
        m_sizingInfo.minSize = sz;
        Q_EMIT minSizeChanged(this);
        setSize_recursive(size().expandedTo(sz));
    }
}

void Item::setMaxSizeHint(QSize sz)
{
    if (sz != m_sizingInfo.maxSizeHint) {
        m_sizingInfo.maxSizeHint = sz;
        Q_EMIT maxSizeChanged(this);
    }
}

QSize Item::minSize() const
{
    return m_sizingInfo.minSize;
}

QSize Item::maxSizeHint() const
{
    return m_sizingInfo.maxSizeHint.boundedTo(QSize(KDDOCKWIDGETS_MAX_WIDTH, KDDOCKWIDGETS_MAX_HEIGHT));
}

void Item::setPos(QPoint pos)
{
    QRect geo = m_sizingInfo.geometry;
    geo.moveTopLeft(pos);
    setGeometry(geo);
}

void Item::setPos(int pos, Qt::Orientation o)
{
    if (o == Qt::Vertical) {
        setPos({ x(), pos });
    } else {
        setPos({ pos, y() });
    }
}

int Item::pos(Qt::Orientation o) const
{
    return o == Qt::Vertical ? y() : x();
}

void Item::insertItem(Item *item, Location loc, DefaultSizeMode defaultSizeMode, AddingOption option)
{
    Q_ASSERT(item != this);

    item->setIsVisible(!(option & AddingOption_StartHidden));
    Q_ASSERT(!((option & AddingOption_StartHidden) && item->isContainer()));

    if (m_parent->hasOrientationFor(loc)) {
        const bool locIsSide1 = locationIsSide1(loc);
        int indexInParent = m_parent->childItems().indexOf(this);
        if (!locIsSide1)
            indexInParent++;

        const Qt::Orientation orientation = orientationForLocation(loc);
        if (orientation != m_parent->orientation()) {
            Q_ASSERT(m_parent->visibleChildren().size() == 1);
            // This is the case where the container only has one item, so it's both vertical and horizontal
            // Now its orientation gets defined
            m_parent->setOrientation(orientation);
        }

        m_parent->insertItem(item, indexInParent, defaultSizeMode);
    } else {
        ItemContainer *container = m_parent->convertChildToContainer(this);
        container->insertItem(item, loc, defaultSizeMode, option);
    }
}

int Item::x() const
{
    return m_sizingInfo.geometry.x();
}

int Item::y() const
{
    return m_sizingInfo.geometry.y();
}

int Item::width() const
{
    return m_sizingInfo.geometry.width();
}

int Item::height() const
{
    return m_sizingInfo.geometry.height();
}

QSize Item::size() const
{
    return m_sizingInfo.geometry.size();
}

void Item::setSize(QSize sz)
{
    QRect newGeo = m_sizingInfo.geometry;
    newGeo.setSize(sz);
    setGeometry(newGeo);
}

QPoint Item::pos() const
{
    return m_sizingInfo.geometry.topLeft();
}

int Item::position(Qt::Orientation o) const
{
    return o == Qt::Vertical ? y()
                             : x();
}

QRect Item::geometry() const
{
    return isBeingInserted() ? QRect()
                             : m_sizingInfo.geometry;
}

QRect Item::rect() const
{
    return QRect(0, 0, width(), height());
}

bool Item::isContainer() const
{
    return m_isContainer;
}

int Item::minLength(Qt::Orientation o) const
{
    return Layouting::length(minSize(), o);
}

int Item::maxLengthHint(Qt::Orientation o) const
{
    return Layouting::length(maxSizeHint(), o);
}

void Item::setLength(int length, Qt::Orientation o)
{
    Q_ASSERT(length > 0);
    if (o == Qt::Vertical) {
        const int w = qMax(width(), hardcodedMinimumSize.width());
        setSize(QSize(w, length));
    } else {
        const int h = qMax(height(), hardcodedMinimumSize.height());
        setSize(QSize(length, h));
    }
}

void Item::setLength_recursive(int length, Qt::Orientation o)
{
    setLength(length, o);
}

int Item::length(Qt::Orientation o) const
{
    return Layouting::length(size(), o);
}

int Item::availableLength(Qt::Orientation o) const
{
    return length(o) - minLength(o);
}

bool Item::isPlaceholder() const
{
    return !isVisible();
}

bool Item::isVisible(bool excludeBeingInserted) const
{
    return m_isVisible && !(excludeBeingInserted && isBeingInserted());
}

void Item::setIsVisible(bool is)
{
    if (is != m_isVisible) {
        m_isVisible = is;
        Q_EMIT visibleChanged(this, is);
    }

    if (is && m_guest) {
        m_guest->setGeometry(mapToRoot(rect()));
        m_guest->setVisible(true); // TODO: Only set visible when apply*() ?
    }

    updateObjectName();
}

void Item::setGeometry_recursive(QRect rect)
{
    // Recursiveness doesn't apply for non-container items
    setGeometry(rect);
}

bool Item::checkSanity()
{
    if (!root())
        return true;

    if (minSize().width() > width() || minSize().height() > height()) {
        root()->dumpLayout();
        qWarning() << Q_FUNC_INFO << "Size constraints not honoured" << this
                   << "; min=" << minSize() << "; size=" << size();
        return false;
    }

    if (m_guest) {
        if (m_guest->parent() != hostWidget()->asQObject()) {
            qWarning() << Q_FUNC_INFO << "Unexpected parent for our guest"
                       << m_guest->parent() << "; host=" << hostWidget()
                       << m_guest->asQObject() << this;
            return false;
        }

        if (false && !m_guest->isVisible() && (!m_guest->parent() || m_guest->parentWidget()->isVisible())) {
            // TODO: if guest is explicitly hidden we're not hidding the item yet
            qWarning() << Q_FUNC_INFO << "Guest widget isn't visible" << this
                       << m_guest->asQObject();
            return false;
        }

        if (m_guest->geometry() != mapToRoot(rect())) {
            root()->dumpLayout();
            auto d = qWarning();
            d << Q_FUNC_INFO << "Guest widget doesn't have correct geometry. has"
              << "guest.global=" << m_guest->geometry()
              << "; item.local=" << geometry()
              << "; item.global=" << mapToRoot(rect())
              << this;
            m_guest->dumpDebug(d);

            return false;
        }
    }
return true;
    if (!isVisible()) {
        if (m_guest && m_guest->isVisible()) {
            qWarning() << Q_FUNC_INFO << "Item is not visible but guest is visible";
            return false;
        }
    }

    return true;
}

void Item::setGeometry(QRect rect)
{
    QRect &m_geometry = m_sizingInfo.geometry;

    if (rect != m_geometry) {
        const QRect oldGeo = m_geometry;

        m_geometry = rect;

        if (rect.isEmpty()) {
            // Just a sanity check...
            ItemContainer *c = asContainer();
            if (c) {
                if (c->hasVisibleChildren()) {
                    if (auto r = root()) r->dumpLayout();
                    Q_ASSERT(false);
                }
            } else {
                qWarning() << Q_FUNC_INFO << "Empty rect";
            }
        }

        const QSize minSz = minSize();
        if (rect.width() < minSz.width() || rect.height() < minSz.height()) {
            if (auto r = root()) r->dumpLayout();
            qWarning() << Q_FUNC_INFO << this << "Constraints not honoured."
                       << "sz=" << rect.size() << "; min=" << minSz
                       << ": parent=" << parentContainer();

        }

        Q_EMIT geometryChanged();

        if (oldGeo.x() != x())
            Q_EMIT xChanged();
        if (oldGeo.y() != y())
            Q_EMIT yChanged();
        if (oldGeo.width() != width())
            Q_EMIT widthChanged();
        if (oldGeo.height() != height())
            Q_EMIT heightChanged();

        updateWidgetGeometries();
    }
}

void Item::dumpLayout(int level)
{
    QString indent;
    indent.fill(QLatin1Char(' '), level);

    auto dbg = qDebug().noquote();

    dbg  << indent << "- Widget: " << objectName()
         << m_sizingInfo.geometry// << "r=" << m_geometry.right() << "b=" << m_geometry.bottom()
         << "; min=" << minSize();

    if (maxSizeHint() != QSize(KDDOCKWIDGETS_MAX_WIDTH, KDDOCKWIDGETS_MAX_HEIGHT))
        dbg << "; max=" << maxSizeHint();

    if (!isVisible())
        dbg << QStringLiteral(";hidden;");

    if (m_guest && geometry() != m_guest->geometry()) {
        dbg << "; guest geometry=" << m_guest->geometry();
    }

    if (m_sizingInfo.isBeingInserted)
        dbg << QStringLiteral(";beingInserted;");

    dbg << this << "; guest=" << guestAsQObject();
}

Item::Item(Widget *hostWidget, ItemContainer *parent)
    : QObject(parent)
    , m_isContainer(false)
    , m_parent(parent)
    , m_hostWidget(hostWidget)
{
    connectParent(parent);
}

Item::Item(bool isContainer, Widget *hostWidget, ItemContainer *parent)
    : QObject(parent)
    , m_isContainer(isContainer)
    , m_parent(parent)
    , m_hostWidget(hostWidget)
{
    connectParent(parent);
}

Item::~Item()
{
}

bool Item::eventFilter(QObject *widget, QEvent *e)
{
    if (e->type() != QEvent::ParentChange)
        return false;

    QObject *host = hostWidget() ? hostWidget()->asQObject() : nullptr;
    if (widget->parent() != host) {
        // Frame was detached into floating window. Turn into placeholder
        Q_ASSERT(isVisible());
        turnIntoPlaceholder();
    }

    return false;
}


void Item::turnIntoPlaceholder()
{
    Q_ASSERT(!isContainer());

    // Turning into placeholder just means hidding it. So we can show it again in its original position.
    // Call removeItem() so we share the code for making the neighbours grow into the space that becomes available
    // after hidding this one
    parentContainer()->removeItem(this, /*hardDelete=*/ false);
}

void Item::updateObjectName()
{
    if (isContainer())
        return;

    if (auto w = guestAsQObject()) {
        setObjectName(w->objectName().isEmpty() ? QStringLiteral("widget") : w->objectName());
    } else if (!isVisible()) {
        setObjectName(QStringLiteral("hidden"));
    } else if (!m_guest) {
        setObjectName(QStringLiteral("null"));
    } else {
        setObjectName(QStringLiteral("empty"));
    }
}

void Item::onWidgetDestroyed()
{
    m_guest = nullptr;

    if (m_refCount) {
        turnIntoPlaceholder();
    } else if (!isRoot()) {
        parentContainer()->removeItem(this);
    }
}

void Item::onWidgetLayoutRequested()
{
    if (Widget *w = guestWidget()) {
        if (w->size() != size()) {
            qDebug() << Q_FUNC_INFO << "TODO: Not implemented yet. Widget can't just decide to resize yet"
                       << w->size()
                       << size()
                       << m_sizingInfo.geometry
                       << m_sizingInfo.isBeingInserted;
        }

        if (w->minSize() != minSize()) {
            setMinSize(m_guest->minSize());
        }

        setMaxSizeHint(w->maxSizeHint());
    }
}

bool Item::isRoot() const
{
    return m_parent == nullptr;
}

int Item::visibleCount_recursive() const
{
    return isVisible() ? 1 : 0;
}

struct ItemContainer::Private
{
    Private(ItemContainer *q)
        : q(q)
    {
        (void) Config::self(); // Ensure Config ctor runs, as it registers qml types
    }

    int defaultLengthFor(Item *item, DefaultSizeMode) const;
    bool isOverflowing() const;
    void relayoutIfNeeded();
    const Item *itemFromPath(const QVector<int> &path) const;
    void resizeChildren(QSize oldSize, QSize newSize, SizingInfo::List &sizes, ChildrenResizeStrategy);
    void honourMaxSizes(SizingInfo::List &sizes);
    void scheduleCheckSanity() const;
    Separator *neighbourSeparator(const Item *item, Side, Qt::Orientation) const;
    Separator *neighbourSeparator_recursive(const Item *item, Side, Qt::Orientation) const;
    void updateWidgets_recursive();
    /// Returns the positions that each separator should have (x position if Qt::Horizontal, y otherwise)
    QVector<int> requiredSeparatorPositions() const;
    void updateSeparators();
    void deleteSeparators();
    Separator* separatorAt(int p) const;
    QVector<double> childPercentages() const;
    bool isDummy() const;
    void deleteSeparators_recursive();
    void updateSeparators_recursive();
    QSize minSize(const Item::List &items) const;
    int excessLength() const;

    mutable bool m_checkSanityScheduled = false;
    QVector<Layouting::Separator*> m_separators;
    bool m_convertingItemToContainer = false;
    bool m_blockUpdatePercentages = false;
    bool m_isDeserializing = false;
    bool m_isSimplifying = false;
    Qt::Orientation m_orientation = Qt::Vertical;
    Item::List m_children;
    ItemContainer *const q;
};

ItemContainer::ItemContainer(Widget *hostWidget, ItemContainer *parent)
    : Item(true, hostWidget, parent)
    , d(new Private(this))
{
    Q_ASSERT(parent);

    connect(this, &Item::xChanged, this, [this] {
        for (Item *item : qAsConst(d->m_children)) {
            Q_EMIT item->xChanged();
        }
    });

    connect(this, &Item::yChanged, this, [this] {
        for (Item *item : qAsConst(d->m_children)) {
            Q_EMIT item->yChanged();
        }
    });
}

ItemContainer::ItemContainer(Widget *hostWidget)
    : Item(true, hostWidget, /*parentContainer=*/ nullptr)
    , d(new Private(this))
{
}

ItemContainer::~ItemContainer()
{
    delete d;
}

bool ItemContainer::checkSanity()
{
    d->m_checkSanityScheduled = false;

    if (!hostWidget()) {
        /// This is a dummy ItemContainer, just return true
        return true;
    }

    if (!Item::checkSanity())
        return false;

    if (numChildren() == 0 && !isRoot()) {
        qWarning() << Q_FUNC_INFO << "Container is empty. Should be deleted";
        return false;
    }

    if (d->m_orientation != Qt::Vertical && d->m_orientation != Qt::Horizontal) {
        qWarning() << Q_FUNC_INFO << "Invalid orientation" << d->m_orientation << this;
        return false;
    }

    // Check that the geometries don't overlap
    int expectedPos = 0;
    for (Item *item : qAsConst(d->m_children)) {
        if (!item->isVisible())
            continue;
        const int pos = Layouting::pos(item->pos(), d->m_orientation);
        if (expectedPos != pos) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Unexpected pos" << pos << "; expected=" << expectedPos
                       << "; for item=" << item
                       << "; isContainer=" << item->isContainer();
            return false;
        }

        expectedPos = pos + Layouting::length(item->size(), d->m_orientation) + separatorThickness;
    }

    const int h1 = Layouting::length(size(), oppositeOrientation(d->m_orientation));
    for (Item *item : qAsConst(d->m_children)) {
        if (item->parentContainer() != this) {
            qWarning() << "Invalid parent container for" << item
                       << "; is=" << item->parentContainer() << "; expected=" << this;
            return false;
        }

        if (item->parent() != this) {
            qWarning() << "Invalid QObject parent for" << item
                       << "; is=" << item->parent() << "; expected=" << this;
            return false;
        }

        if (item->isVisible()) {
            // Check the children height (if horizontal, and vice-versa)
            const int h2 = Layouting::length(item->size(), oppositeOrientation(d->m_orientation));
            if (h1 != h2) {
                root()->dumpLayout();
                qWarning() << Q_FUNC_INFO << "Invalid size for item." << item
                           << "Container.length=" << h1 << "; item.length=" << h2;
                return false;
            }

            if (!rect().contains(item->geometry())) {
                root()->dumpLayout();
                qWarning() << Q_FUNC_INFO << "Item geo is out of bounds. item=" << item << "; geo="
                           << item->geometry() << "; parent.rect=" << rect();
                return false;
            }
        }

        if (!item->checkSanity())
            return false;
    }

    const Item::List visibleChildren = this->visibleChildren();
    const bool isEmptyRoot = isRoot() && visibleChildren.isEmpty();
    if (!isEmptyRoot) {
        int occupied = qMax(0, Item::separatorThickness * (visibleChildren.size() - 1));
        for (Item *item : visibleChildren) {
            occupied += item->length(d->m_orientation);
        }

        if (occupied != length()) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Unexpected length. Expected=" << occupied
                       << "; got=" << length() << "; this=" << this;
            return false;
        }

        const QVector<double> percentages = d->childPercentages();
        const double totalPercentage = std::accumulate(percentages.begin(), percentages.end(), 0.0);
        const double expectedPercentage = visibleChildren.isEmpty() ? 0.0 : 1.0;
        if (!qFuzzyCompare(totalPercentage, expectedPercentage)) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Percentages don't add up"
                       << totalPercentage << percentages
                       << this;
            const_cast<ItemContainer*>(this)->d->updateSeparators_recursive();
            qWarning() << Q_FUNC_INFO << d->childPercentages();
            return false;
        }
    }

    const int numVisibleChildren = visibleChildren.size();
    if (d->m_separators.size() != qMax(0, numVisibleChildren - 1)) {
        root()->dumpLayout();
        qWarning() << Q_FUNC_INFO << "Unexpected number of separators" << d->m_separators.size()
                   << numVisibleChildren;
        return false;
    }

    const QSize expectedSeparatorSize = isVertical() ? QSize(width(), Item::separatorThickness)
                                                     : QSize(Item::separatorThickness, height());

    const int pos2 = Layouting::pos(mapToRoot(QPoint(0, 0)), oppositeOrientation(d->m_orientation));

    for (int i = 0; i < d->m_separators.size(); ++i) {
        Separator *separator = d->m_separators.at(i);
        Item *item = visibleChildren.at(i);
        const int expectedSeparatorPos = mapToRoot(item->m_sizingInfo.edge(d->m_orientation) + 1, d->m_orientation);

        if (separator->host() != host()) {
            qWarning() << Q_FUNC_INFO << "Invalid host widget for separator"
                       << separator->host() << host() << this;
            return false;
        }

        if (separator->parentContainer() != this) {
            qWarning() << Q_FUNC_INFO << "Invalid parent container for separator"
                       << separator->parentContainer() << separator << this;
            return false;
        }

        if (separator->position() != expectedSeparatorPos) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Unexpected separator position" << separator->position()
                       << "; expected=" << expectedSeparatorPos
                       << separator << "; this=" << this;
            return false;
        }

        Widget *separatorWidget = separator->asWidget();
        if (separatorWidget->geometry().size() != expectedSeparatorSize) {
            qWarning() << Q_FUNC_INFO << "Unexpected separator size" << separatorWidget->geometry().size()
                       << "; expected=" << expectedSeparatorSize
                       << separator << "; this=" << this;
            return false;
        }

        const int separatorPos2 = Layouting::pos(separatorWidget->geometry().topLeft(), oppositeOrientation(d->m_orientation));
        if (Layouting::pos(separatorWidget->geometry().topLeft(), oppositeOrientation(d->m_orientation)) != pos2) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Unexpected position pos2=" << separatorPos2
                       << "; expected=" << pos2
                          << separator << "; this=" << this;
            return false;
        }

        if (separator->host() != host()) {
            qWarning() << Q_FUNC_INFO << "Unexpected host widget in separator"
                       << separator->host() << "; expected=" << host();
            return false;
        }

        // Check that the seprator bounds are correct. We can't always honour widget's max-size constraints, so only honour min-size
        const int separatorMinPos = minPosForSeparator_global(separator, /*honourMax=*/ false);
        const int separatorMaxPos = maxPosForSeparator_global(separator, /*honourMax=*/ false);
        const int separatorPos = separator->position();
        if (separatorPos < separatorMinPos || separatorPos > separatorMaxPos ||
                separatorMinPos < 0 || separatorMaxPos <= 0) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Invalid bounds for separator, pos="
                       << separatorPos << "; min=" << separatorMinPos
                       << "; max=" << separatorMaxPos
                       << separator;
            return false;
        }
    }

#ifdef DOCKS_DEVELOPER_MODE
    // Can cause slowdown, so just use it in developer mode.
    if (isRoot()) {
        if (!asContainer()->test_suggestedRect())
            return false;
    }
#endif

    return true;
}

void ItemContainer::Private::scheduleCheckSanity() const
{
    if (!m_checkSanityScheduled) {
        m_checkSanityScheduled = true;
        QTimer::singleShot(0, q->root(), &ItemContainer::checkSanity);
    }
}

bool ItemContainer::hasOrientation() const
{
    return isVertical() || isHorizontal();
}

int ItemContainer::numChildren() const
{
    return d->m_children.size();
}

int ItemContainer::numVisibleChildren() const
{
    int num = 0;
    for (Item *child : qAsConst(d->m_children)) {
        if (child->isVisible())
            num++;
    }
    return num;
}

int ItemContainer::indexOfVisibleChild(const Item *item) const
{
    const Item::List items = visibleChildren();
    return items.indexOf(const_cast<Item*>(item));
}

const Item::List ItemContainer::childItems() const
{
    return d->m_children;
}

void ItemContainer::removeItem(Item *item, bool hardRemove)
{
    Q_ASSERT(!item->isRoot());

    if (!contains(item)) {
        // Not ours, ask parent
        item->parentContainer()->removeItem(item, hardRemove);
        return;
    }

    Item *side1Item = visibleNeighbourFor(item, Side1);
    Item *side2Item = visibleNeighbourFor(item, Side2);

    const bool isContainer = item->isContainer();
    const bool wasVisible = !isContainer && item->isVisible();

    if (hardRemove) {
        d->m_children.removeOne(item);
        delete item;
        if (!isContainer)
            Q_EMIT root()->numItemsChanged();
    } else {
        item->setIsVisible(false);
        item->setGuestWidget(nullptr);

        if (!wasVisible && !isContainer) {
            // Was already hidden
            return;
        }
    }

    if (wasVisible) {
        Q_EMIT root()->numVisibleItemsChanged(root()->numVisibleChildren());
    }

    if (isEmpty()) {
        // Empty container is useless, delete it
        if (auto p = parentContainer())
            p->removeItem(this, /*hardDelete=*/ true);
    } else if (!hardRemove && !hasVisibleChildren()) {
        if (auto p = parentContainer()) {
            p->removeItem(this, /*hardDelete=*/ false);
            setGeometry(QRect());
        }
    } else {
        // Neighbours will occupy the space of the deleted item
        growNeighbours(side1Item, side2Item);
        Q_EMIT itemsChanged();

        updateSizeConstraints();
        d->updateSeparators_recursive();
    }
}

bool ItemContainer::isEmpty() const
{
    return d->m_children.isEmpty();
}

void ItemContainer::setGeometry_recursive(QRect rect)
{
    setPos(rect.topLeft());

    // Call resize, which is recursive and will resize the children too
    setSize_recursive(rect.size());
}

ItemContainer *ItemContainer::convertChildToContainer(Item *leaf)
{
    QScopedValueRollback<bool> converting(d->m_convertingItemToContainer, true);

    const int index = d->m_children.indexOf(leaf);
    Q_ASSERT(index != -1);
    auto container = new ItemContainer(hostWidget(), this);
    container->setParentContainer(nullptr);
    container->setParentContainer(this);

    insertItem(container, index, DefaultSizeMode::None);
    d->m_children.removeOne(leaf);
    container->setGeometry(leaf->geometry());
    container->insertItem(leaf, Location_OnTop, DefaultSizeMode::None);
    Q_EMIT itemsChanged();
    d->updateSeparators_recursive();

    return container;
}

void ItemContainer::insertItem(Item *item, Location loc, DefaultSizeMode defaultSizeMode,
                               AddingOption addingOption)
{
    Q_ASSERT(item != this);
    if (contains(item)) {
        qWarning() << Q_FUNC_INFO << "Item already exists";
        return;
    }

    item->setIsVisible(!(addingOption & AddingOption_StartHidden));
    Q_ASSERT(!((addingOption & AddingOption_StartHidden) && item->isContainer()));

    const Qt::Orientation locOrientation = orientationForLocation(loc);

    if (hasOrientationFor(loc)) {
        if (d->m_children.size() == 1) {
            // 2 items is the minimum to know which orientation we're layedout
            d->m_orientation = locOrientation;
        }

        const int index = locationIsSide1(loc) ? 0 : d->m_children.size();
        insertItem(item, index, defaultSizeMode);
    } else {
        // Inserting directly in a container ? Only if it's root.
        Q_ASSERT(isRoot());
        auto container = new ItemContainer(hostWidget(), this);
        container->setGeometry(rect());
        container->setChildren(d->m_children, d->m_orientation);
        d->m_children.clear();
        setOrientation(oppositeOrientation(d->m_orientation));
        insertItem(container, 0, DefaultSizeMode::None);

        // Now we have the correct orientation, we can insert
        insertItem(item, loc, defaultSizeMode, addingOption);

        if (!container->hasVisibleChildren())
            container->setGeometry(QRect());
    }

    d->updateSeparators_recursive();
    d->scheduleCheckSanity();
}

void ItemContainer::onChildMinSizeChanged(Item *child)
{
    if (d->m_convertingItemToContainer || d->m_isDeserializing || !child->isVisible()) {
        // Don't bother our parents, we're converting
        return;
    }

    updateSizeConstraints();

    if (child->isBeingInserted())
        return;

    if (numVisibleChildren() == 1 && child->isVisible()) {
        // The easy case. Child is alone in the layout, occupies everything.
        child->setGeometry(rect());
        updateChildPercentages();
        return;
    }

    const QSize missingForChild = child->missingSize();
    if (!missingForChild.isNull()) {
        // Child has some growing to do. It will grow left and right equally, (and top-bottom), as needed.
        growItem(child, Layouting::length(missingForChild, d->m_orientation), GrowthStrategy::BothSidesEqually, NeighbourSqueezeStrategy::AllNeighbours);
    }

    updateChildPercentages();
}

void ItemContainer::updateSizeConstraints()
{
    const QSize missingSize = this->missingSize();
    if (!missingSize.isNull()) {
        if (isRoot()) {
            // Resize the whole layout
            setSize_recursive(size() + missingSize);
        }
    }

    // Our min-size changed, notify our parent, and so on until it reaches root()
    Q_EMIT minSizeChanged(this);
}

void ItemContainer::onChildVisibleChanged(Item *, bool visible)
{
    if (d->m_isDeserializing || isInSimplify())
        return;

    const int numVisible = numVisibleChildren();
    if (visible && numVisible == 1) {
        // Child became visible and there's only 1 visible child. Meaning there were 0 visible before.
        Q_EMIT visibleChanged(this, true);
    } else if (!visible && numVisible == 0) {
        Q_EMIT visibleChanged(this, false);
    }
}

QRect ItemContainer::suggestedDropRect(const Item *item, const Item *relativeTo, Location loc) const
{
    // Returns the drop rect. This is the geometry used by the rubber band when you hover over an indicator.
    // It's calculated by copying the layout and inserting the item into the dummy/invisible copy
    // The we see which geometry the item got. This way the returned geometry is always what the item will get
    // if you drop it.
    // One exception is if the window doesn't have enough space and it would grow. In this case
    // we fall back to something reasonable


    if (relativeTo && !relativeTo->parentContainer()) {
        qWarning() << Q_FUNC_INFO << "No parent container";
        return {};
    }

    if (relativeTo && relativeTo->parentContainer() != this) {
        qWarning() << Q_FUNC_INFO << "Called on the wrong container";
        return {};
    }

    if (relativeTo && !relativeTo->isVisible()) {
        qWarning() << Q_FUNC_INFO << "relative to isn't visible";
        return {};
    }

    if (loc == Location_None) {
        qWarning() << Q_FUNC_INFO << "Invalid location";
        return {};
    }

    const QSize availableSize = root()->availableSize();
    const QSize minSize = item->minSize();
    const bool isEmpty = !root()->hasVisibleChildren();
    const int extraWidth = (isEmpty || locationIsVertical(loc)) ? 0 : Item::separatorThickness;
    const int extraHeight = (isEmpty || !locationIsVertical(loc)) ? 0 : Item::separatorThickness;
    const bool windowNeedsGrowing = availableSize.width() < minSize.width() + extraWidth ||
                                    availableSize.height() < minSize.height() + extraHeight;

    if (windowNeedsGrowing)
        return suggestedDropRectFallback(item, relativeTo, loc);

    const QVariantMap rootSerialized = root()->toVariantMap();
    ItemContainer rootCopy(nullptr);
    rootCopy.fillFromVariantMap(rootSerialized, {});

    if (relativeTo)
        relativeTo = rootCopy.d->itemFromPath(relativeTo->pathFromRoot());

    const QVariantMap itemSerialized = item->toVariantMap();
    auto itemCopy = new Item(nullptr);
    itemCopy->fillFromVariantMap(itemSerialized, {});

    if (relativeTo) {
        auto r = const_cast<Item*>(relativeTo);
        r->insertItem(itemCopy, loc, DefaultSizeMode::FairButFloor);
    } else {
        rootCopy.insertItem(itemCopy, loc, DefaultSizeMode::FairButFloor);
    }

    if (rootCopy.size() != root()->size()) {
        // Doesn't happen
        qWarning() << Q_FUNC_INFO << "The root copy grew ?!" << rootCopy.size() << root()->size()
                   << loc;
        return suggestedDropRectFallback(item, relativeTo, loc);
    }

    return itemCopy->mapToRoot(itemCopy->rect());
}

QRect ItemContainer::suggestedDropRectFallback(const Item *item, const Item *relativeTo, Location loc) const
{
    const QSize minSize = item->minSize();
    const int itemMin = Layouting::length(minSize, d->m_orientation);
    const int available = availableLength() - Item::separatorThickness;
    if (relativeTo) {
        int suggestedPos = 0;
        const QRect relativeToGeo = relativeTo->geometry();
        const int suggestedLength = relativeTo->length(orientationForLocation(loc)) / 2;
        switch (loc) {
        case Location_OnLeft:
            suggestedPos = relativeToGeo.x();
            break;
        case Location_OnTop:
            suggestedPos = relativeToGeo.y();
            break;
        case Location_OnRight:
            suggestedPos = relativeToGeo.right() - suggestedLength + 1;
            break;
        case Location_OnBottom:
            suggestedPos = relativeToGeo.bottom() - suggestedLength + 1;
            break;
        default:
            Q_ASSERT(false);
        }

        QRect rect;
        if (orientationForLocation(loc) == Qt::Vertical) {
            rect.setTopLeft(QPoint(relativeTo->x(), suggestedPos));
            rect.setSize(QSize(relativeTo->width(), suggestedLength));
        } else {
            rect.setTopLeft(QPoint(suggestedPos, relativeTo->y()));
            rect.setSize(QSize(suggestedLength, relativeTo->height()));
        }

        return mapToRoot(rect);
    } else if (isRoot()) {
        // Relative to the window itself
        QRect rect = this->rect();
        const int oneThird = length() / 3;
        const int suggestedLength = qMax(qMin(available, oneThird), itemMin);

        switch (loc) {
        case Location_OnLeft:
            rect.setWidth(suggestedLength);
            break;
        case Location_OnTop:
            rect.setHeight(suggestedLength);
            break;
        case Location_OnRight:
            rect.adjust(rect.width() - suggestedLength, 0, 0, 0);
            break;
        case Location_OnBottom:
            rect.adjust(0, rect.bottom() - suggestedLength, 0, 0);
            break;
        case Location_None:
            return {};
        }

        return rect;

    } else {
        qWarning() << Q_FUNC_INFO << "Shouldn't happen";
    }

    return {};
}

void ItemContainer::positionItems()
{
    SizingInfo::List sizes = this->sizes();
    positionItems(/*by-ref=*/sizes);
    applyPositions(sizes);

    d->updateSeparators_recursive();
}

void ItemContainer::positionItems_recursive()
{
    positionItems();
    for (Item *item : d->m_children) {
        if (item->isVisible()) {
            if (auto c = item->asContainer())
                c->positionItems_recursive();
        }
    }
}

void ItemContainer::applyPositions(const SizingInfo::List &sizes)
{
    const Item::List items = visibleChildren();
    const int count = items.size();
    Q_ASSERT(count == sizes.size());
    for (int i = 0; i < count; ++i) {
        Item *item = items.at(i);
        const SizingInfo &sizing = sizes[i];
        if (sizing.isBeingInserted) {
            continue;
        }

        const Qt::Orientation oppositeOrientation = ::oppositeOrientation(d->m_orientation);
        // If the layout is horizontal, the item will have the height of the container. And vice-versa
        item->setLength_recursive(sizing.length(oppositeOrientation), oppositeOrientation);

        item->setPos(sizing.geometry.topLeft());
    }
}

Qt::Orientation ItemContainer::orientation() const
{
    return d->m_orientation;
}

void ItemContainer::positionItems(SizingInfo::List &sizes)
{
    int nextPos = 0;
    const int count = sizes.count();
    const Qt::Orientation oppositeOrientation = ::oppositeOrientation(d->m_orientation);
    for (int i = 0; i < count; ++i) {
        SizingInfo &sizing = sizes[i];
        if (sizing.isBeingInserted) {
            nextPos += Item::separatorThickness;
            continue;
        }

        // If the layout is horizontal, the item will have the height of the container. And vice-versa
        const int oppositeLength = Layouting::length(size(), oppositeOrientation);
        sizing.setLength(oppositeLength, oppositeOrientation);
        sizing.setPos(0, oppositeOrientation);

        sizing.setPos(nextPos, d->m_orientation);
        nextPos += sizing.length(d->m_orientation) + Item::separatorThickness;
    }
}

void ItemContainer::clear()
{
    for (Item *item : qAsConst(d->m_children)) {
        if (ItemContainer *container = item->asContainer())
            container->clear();

        delete item;
    }
    d->m_children.clear();
    d->deleteSeparators();
}

Item* ItemContainer::itemForObject(const QObject *o) const
{
    for (Item *item : d->m_children) {
        if (item->isContainer()) {
            if (Item *result = item->asContainer()->itemForObject(o))
                return result;
        } else if (auto guest = item->guestWidget()) {
            if (guest && guest->asQObject() == o)
                return item;
        }
    }

    return nullptr;
}

Item *ItemContainer::itemForWidget(const Widget *w) const
{
    for (Item *item : qAsConst(d->m_children)) {
        if (item->isContainer()) {
            if (Item *result = item->asContainer()->itemForWidget(w))
                return result;
        } else if (item->guestWidget() == w) {
            return item;
        }
    }

    return nullptr;
}

int ItemContainer::visibleCount_recursive() const
{
    int count = 0;
    for (Item *item : qAsConst(d->m_children)) {
        count += item->visibleCount_recursive();
    }

    return count;
}

int ItemContainer::count_recursive() const
{
    int count = 0;
    for (Item *item : qAsConst(d->m_children)) {
        if (auto c = item->asContainer()) {
            count += c->count_recursive();
        } else {
            count++;
        }
    }

    return count;
}

Item *ItemContainer::itemAt(QPoint p) const
{
    for (Item *item : qAsConst(d->m_children)) {
        if (item->isVisible() && item->geometry().contains(p))
            return item;
    }

    return nullptr;
}

Item *ItemContainer::itemAt_recursive(QPoint p) const
{
    if (Item *item = itemAt(p)) {
        if (auto c = item->asContainer()) {
            return c->itemAt_recursive(c->mapFromParent(p));
        } else {
            return item;
        }
    }

    return nullptr;
}

Item::List ItemContainer::items_recursive() const
{
   Item::List items;
   items.reserve(30); // sounds like a good upper number to minimize allocations
   for (Item *item : qAsConst(d->m_children)) {
       if (auto c  = item->asContainer()) {
           items << c->items_recursive();
       } else {
           items << item;
       }
   }

   return items;
}

void ItemContainer::setHostWidget(Widget *host)
{
    Item::setHostWidget(host);
    d->deleteSeparators_recursive();
    for (Item *item : qAsConst(d->m_children)) {
        item->setHostWidget(host);
    }

    d->updateSeparators_recursive();
}

void ItemContainer::setIsVisible(bool)
{
    // no-op for containers, visibility is calculated
}

bool ItemContainer::isVisible(bool excludeBeingInserted) const
{
    return hasVisibleChildren(excludeBeingInserted);
}

void ItemContainer::setLength_recursive(int length, Qt::Orientation o)
{
    QSize sz = size();
    if (o == Qt::Vertical) {
        sz.setHeight(length);
    } else {
        sz.setWidth(length);
    }

    setSize_recursive(sz);
}

void ItemContainer::insertItem(Item *item, int index, DefaultSizeMode defaultSizeMode)
{
    if (defaultSizeMode != DefaultSizeMode::None) {
        /// Choose a nice size for the item we're adding
        const int suggestedLength = d->defaultLengthFor(item, defaultSizeMode);
        item->setLength_recursive(suggestedLength, d->m_orientation);
    }

    d->m_children.insert(index, item);
    item->setParentContainer(this);

    Q_EMIT itemsChanged();

    if (!d->m_convertingItemToContainer && item->isVisible())
        restoreChild(item);

    const bool shouldEmitVisibleChanged = item->isVisible();

    if (!d->m_convertingItemToContainer && !s_inhibitSimplify)
        simplify();

    if (shouldEmitVisibleChanged)
        Q_EMIT root()->numVisibleItemsChanged(root()->numVisibleChildren());
    Q_EMIT root()->numItemsChanged();
}

bool ItemContainer::hasChildren() const
{
    return !d->m_children.isEmpty();
}

bool ItemContainer::hasVisibleChildren(bool excludeBeingInserted) const
{
    for (Item *item : d->m_children) {
        if (item->isVisible(excludeBeingInserted))
            return true;
    }

    return false;
}

bool ItemContainer::hasOrientationFor(Location loc) const
{
    if (d->m_children.size() <= 1)
        return true;

    return d->m_orientation == orientationForLocation(loc);
}

Item::List ItemContainer::visibleChildren(bool includeBeingInserted) const
{
    Item::List items;
    items.reserve(d->m_children.size());
    for (Item *item : qAsConst(d->m_children)) {
        if (includeBeingInserted) {
            if (item->isVisible() || item->isBeingInserted())
                items << item;
        } else {
            if (item->isVisible() && !item->isBeingInserted())
                items << item;
        }
    }

    return items;
}

int ItemContainer::usableLength() const
{
    const Item::List children = visibleChildren();
    const int numVisibleChildren = children.size();

    if (children.size() <= 1)
        return Layouting::length(size(), d->m_orientation);

    const int separatorWaste = separatorThickness * (numVisibleChildren - 1);
    return length() - separatorWaste;
}

bool ItemContainer::hasSingleVisibleItem() const
{
    return numVisibleChildren() == 1;
}

bool ItemContainer::contains(const Item *item) const
{
    return d->m_children.contains(const_cast<Item *>(item));
}

bool ItemContainer::contains_recursive(const Item *item) const
{
    for (Item *it : qAsConst(d->m_children)) {
        if (it == item) {
            return true;
        } else if (it->isContainer()) {
            if (it->asContainer()->contains_recursive(item))
                return true;
        }
    }

    return false;
}

void ItemContainer::setChildren(const Item::List children, Qt::Orientation o)
{
    d->m_children = children;
    for (Item *item : children)
        item->setParentContainer(this);

    setOrientation(o);
}

void ItemContainer::setOrientation(Qt::Orientation o)
{
    if (o != d->m_orientation) {
        d->m_orientation = o;
        d->updateSeparators_recursive();
    }
}

QSize ItemContainer::Private::minSize(const Item::List &items) const
{
    int minW = 0;
    int minH = 0;
    int numVisible = 0;
    if (!m_children.isEmpty()) {
        for (Item *item : items) {
            if (!(item->isVisible() || item->isBeingInserted()))
                continue;
            numVisible++;
            if (q->isVertical()) {
                minW = qMax(minW, item->minSize().width());
                minH += item->minSize().height();
            } else {
                minH = qMax(minH, item->minSize().height());
                minW += item->minSize().width();
            }
        }

        const int separatorWaste = qMax(0, (numVisible - 1) * separatorThickness);
        if (q->isVertical())
            minH += separatorWaste;
        else
            minW += separatorWaste;
    }

    return QSize(minW, minH);
}

QSize ItemContainer::minSize() const
{
    return d->minSize(d->m_children);
}

QSize ItemContainer::maxSizeHint() const
{
    int maxW = isVertical() ? KDDOCKWIDGETS_MAX_WIDTH : 0;
    int maxH = isVertical() ? 0 : KDDOCKWIDGETS_MAX_HEIGHT;

    const Item::List visibleChildren = this->visibleChildren(/*includeBeingInserted=*/ false);
    if (!visibleChildren.isEmpty()) {
        for (Item *item : visibleChildren) {
            if (item->isBeingInserted())
                continue;
            const QSize itemMaxSz = item->maxSizeHint();
            const int itemMaxWidth = itemMaxSz.width();
            const int itemMaxHeight = itemMaxSz.height();
            if (isVertical()) {
                maxW = qMin(maxW, itemMaxWidth);
                maxH = qMin(maxH + itemMaxHeight, KDDOCKWIDGETS_MAX_HEIGHT);
            } else {
                maxH = qMin(maxH, itemMaxHeight);
                maxW = qMin(maxW + itemMaxWidth, KDDOCKWIDGETS_MAX_WIDTH);
            }
        }

        const int separatorWaste = (visibleChildren.size() - 1) * separatorThickness;
        if (isVertical()) {
            maxH = qMin(maxH + separatorWaste, KDDOCKWIDGETS_MAX_HEIGHT);
        } else {
            maxW = qMin(maxW + separatorWaste, KDDOCKWIDGETS_MAX_WIDTH);
        }
    }

    if (maxW == 0)
        maxW = KDDOCKWIDGETS_MAX_WIDTH;

    if (maxH == 0)
        maxH = KDDOCKWIDGETS_MAX_HEIGHT;

    return QSize(maxW, maxH).expandedTo(d->minSize(visibleChildren));
}

void ItemContainer::Private::resizeChildren(QSize oldSize, QSize newSize, SizingInfo::List &childSizes,
                                            ChildrenResizeStrategy strategy)
{
    // This container is being resized to @p newSize, so we must resize our children too, based
    //on @p strategy.
    // The new sizes are applied to @p childSizes, which will be applied to the widgets when we're done

    const QVector<double> childPercentages = this->childPercentages();
    const int count = childSizes.count();
    const bool widthChanged = oldSize.width() != newSize.width();
    const bool heightChanged = oldSize.height() != newSize.height();
    const bool lengthChanged = (q->isVertical() && heightChanged) || (q->isHorizontal() && widthChanged);
    const int totalNewLength = q->usableLength();

    if (strategy == ChildrenResizeStrategy::Percentage) {
        // In this strategy mode, each children will preserve its current relative size. So, if a child
        // is occupying 50% of this container, then it will still occupy that after the container resize

        int remaining = totalNewLength;
        for (int i = 0; i < count; ++i) {
            const bool isLast = i == count - 1;

            SizingInfo &itemSize = childSizes[i];

            const qreal childPercentage = childPercentages.at(i);
            const int newItemLength = lengthChanged ? (isLast ? remaining
                                                              : int(childPercentage * totalNewLength))
                                                    : itemSize.length(m_orientation);

            if (newItemLength <= 0) {
                q->root()->dumpLayout();
                qWarning() << Q_FUNC_INFO << "Invalid resize newItemLength=" << newItemLength;
                Q_ASSERT(false);
                return;
            }

            remaining = remaining - newItemLength;

            if (q->isVertical()) {
                itemSize.geometry.setSize({ q->width(), newItemLength });
            } else {
                itemSize.geometry.setSize({ newItemLength, q->height() });
            }
        }
    } else if (strategy == ChildrenResizeStrategy::Side1SeparatorMove ||
               strategy == ChildrenResizeStrategy::Side2SeparatorMove) {
        int remaining = Layouting::length(newSize - oldSize, m_orientation); // This is how much we need to give to children (when growing the container), or to take from them when shrinking the container
        const bool isGrowing = remaining > 0;
        remaining = qAbs(remaining); // Easier to deal in positive numbers

        // We're resizing the container, and need to decide if we start resizing the 1st children or in reverse order.
        // If the separator is being dragged left or top, then isSide1SeparatorMove is true.
        // If isSide1SeparatorMove is true and we're growing, then it means this container is on the right/bottom of the separator,
        // so should resize its first children first. Same logic for the other 3 cases

        const bool isSide1SeparatorMove = strategy == ChildrenResizeStrategy::Side1SeparatorMove;
        bool resizeHeadFirst = false;
        if (isGrowing && isSide1SeparatorMove) {
            resizeHeadFirst = true;
        } else if (isGrowing && !isSide1SeparatorMove) {
            resizeHeadFirst = false;
        } else if (!isGrowing && isSide1SeparatorMove) {
            resizeHeadFirst = false;
        } else if (!isGrowing && !isSide1SeparatorMove) {
            resizeHeadFirst = true;
        }

        for (int i = 0; i < count; i++) {
            const int index = resizeHeadFirst ? i : count - 1 - i;

            SizingInfo &size = childSizes[index];

            if (isGrowing) {
                // Since we don't honour item max-size yet, it can just grow all it wants
                size.incrementLength(remaining, m_orientation);
                remaining = 0; // and we're done, the first one got everything
            } else {
                const int availableToGive = size.availableLength(m_orientation);
                const int took = qMin(availableToGive, remaining);
                size.incrementLength(-took, m_orientation);
                remaining -= took;
            }

            if (remaining == 0)
                break;
        }
    }
    honourMaxSizes(childSizes);
}

void ItemContainer::Private::honourMaxSizes(SizingInfo::List &sizes)
{
    // Reduces the size of all children that are bigger than max-size.
    // Assuming there's widgets that are willing to grow to occupy that space.

    int amountNeededToShrink = 0;
    int amountAvailableToGrow = 0;
    QVector<int> indexesOfShrinkers;
    QVector<int> indexesOfGrowers;

    for (int i = 0; i < sizes.count(); ++i) {
        SizingInfo &info = sizes[i];
        const int neededToShrink = info.neededToShrink(m_orientation);
        const int availableToGrow = info.availableToGrow(m_orientation);

        if (neededToShrink > 0) {
            amountNeededToShrink += neededToShrink;
            indexesOfShrinkers.push_back(i);
        } else if (availableToGrow > 0) {
            amountAvailableToGrow = qMin(amountAvailableToGrow + availableToGrow, q->length());
            indexesOfGrowers.push_back(i);
        }
    }

    // Don't grow more than what's needed
    amountAvailableToGrow = qMin(amountNeededToShrink, amountAvailableToGrow);

    // Don't shrink more than what's available to grow
    amountNeededToShrink = qMin(amountAvailableToGrow, amountNeededToShrink);

    if (amountNeededToShrink == 0 || amountAvailableToGrow == 0)
        return;

    // We gathered who needs to shrink and who can grow, now try to do it evenly so that all
    // growers participate, and not just one giving everything.

    // Do the growing:
    while (amountAvailableToGrow > 0) {
        // Each grower will grow a bit (round-robin)
        int toGrow = qMax(1, amountAvailableToGrow / indexesOfGrowers.size());

        for (auto it = indexesOfGrowers.begin(); it != indexesOfGrowers.end();) {
            const int index = *it;
            SizingInfo &sizing = sizes[index];
            const int grew = qMin(sizing.availableToGrow(m_orientation), toGrow);
            sizing.incrementLength(grew, m_orientation);
            amountAvailableToGrow -= grew;

            if (amountAvailableToGrow == 0) {
                // We're done growing
                break;
            }

            if (sizing.availableToGrow(m_orientation) == 0) {
                // It's no longer a grower
                it = indexesOfGrowers.erase(it);
            } else {
                it++;
            }
        }
    }

    // Do the shrinking:
    while (amountNeededToShrink > 0) {
        // Each shrinker will shrink a bit (round-robin)
        int toShrink = qMax(1, amountNeededToShrink / indexesOfShrinkers.size());

        for (auto it = indexesOfShrinkers.begin(); it != indexesOfShrinkers.end();) {
            const int index = *it;
            SizingInfo &sizing = sizes[index];
            const int shrunk = qMin(sizing.neededToShrink(m_orientation), toShrink);
            sizing.incrementLength(-shrunk, m_orientation);
            amountNeededToShrink -= shrunk;

            if (amountNeededToShrink == 0) {
                // We're done shrinking
                break;
            }

            if (sizing.neededToShrink(m_orientation) == 0) {
                // It's no longer a shrinker
                it = indexesOfShrinkers.erase(it);
            } else {
                it++;
            }
        }
    }
}

void ItemContainer::setSize_recursive(QSize newSize, ChildrenResizeStrategy strategy)
{
    QScopedValueRollback<bool> block(d->m_blockUpdatePercentages, true);

    const QSize minSize = this->minSize();
    if (newSize.width() < minSize.width() || newSize.height() < minSize.height()) {
        root()->dumpLayout();
        qWarning() << Q_FUNC_INFO << "New size doesn't respect size constraints"
                   << "; new=" << newSize
                   << "; min=" << minSize
                   << this;
        return;
    }
    if (newSize == size())
        return;

    const QSize oldSize = size();
    setSize(newSize);

    const Item::List children = visibleChildren();
    const int count = children.size();
    SizingInfo::List childSizes = sizes();

    // #1 Since we changed size, also resize out children.
    // But apply them to our SizingInfo::List first before setting actual Item/QWidget geometries
    // Because we need step #2 where we ensure min sizes for each item are respected. We could
    // calculate and do everything in a single-step, but we already have the code for #2 in growItem()
    // so doing it in 2 steps will reuse much logic.


    // the sizes:
    d->resizeChildren(oldSize, newSize, /*by-ref*/ childSizes, strategy);

    // the positions:
    positionItems(/*by-ref*/ childSizes);

    // #2 Adjust sizes so that each item has at least Item::minSize.
    for (int i = 0; i < count; ++i) {
        SizingInfo &size = childSizes[i];
        const int missing = size.missingLength(d->m_orientation);
        if (missing > 0)
            growItem(i, childSizes, missing, GrowthStrategy::BothSidesEqually, NeighbourSqueezeStrategy::AllNeighbours);
    }

    // #3 Sizes are now correct and honour min/max sizes. So apply them to our Items
    applyGeometries(childSizes, strategy);
}

int ItemContainer::length() const
{
    return isVertical() ? height() : width();
}

QRect ItemContainer::rect() const
{
    QRect rect = m_sizingInfo.geometry;
    rect.moveTo(QPoint(0, 0));
    return rect;
}

void ItemContainer::dumpLayout(int level)
{
    if (level == 0 && hostWidget()) {

        const auto screens = qApp->screens();
        for (auto screen : screens) {
            qDebug().noquote() << "Screen" << screen->geometry() << screen->availableGeometry()
                               << "; drp=" << screen->devicePixelRatio();
        }

        hostWidget()->dumpDebug(qDebug().noquote());
    }

    QString indent;
    indent.fill(QLatin1Char(' '), level);
    const QString beingInserted = m_sizingInfo.isBeingInserted ? QStringLiteral("; beingInserted;")
                                                               : QString();
    const QString visible = !isVisible() ? QStringLiteral(";hidden;")
                                         : QString();

    const QString typeStr = isRoot() ? QStringLiteral("* Root: ")
                                     : QStringLiteral("* Layout: ");

    {
        auto dbg = qDebug().noquote();
        dbg << indent << typeStr << d->m_orientation
            << m_sizingInfo.geometry /*<< "r=" << m_geometry.right() << "b=" << m_geometry.bottom()*/
            << "; min=" << minSize()
            << "; this=" << this << beingInserted << visible
            << "; %=" << d->childPercentages();

        if (maxSizeHint() != Item::hardcodedMaximumSize)
            dbg << "; max=" << maxSizeHint();
    }

    int i = 0;
    for (Item *item : qAsConst(d->m_children)) {
        item->dumpLayout(level + 1);
        if (item->isVisible()) {
            if (i < d->m_separators.size()) {
                auto separator = d->m_separators.at(i);
                qDebug().noquote() << indent << " - Separator: " << "local.geo=" << mapFromRoot(separator->asWidget()->geometry())
                                   << "global.geo=" << separator->asWidget()->geometry()
                                   << separator;
            }
            ++i;
        }
    }
}

void ItemContainer::updateChildPercentages()
{
    if (d->m_blockUpdatePercentages)
        return;

    const int usable = usableLength();
    for (Item *item : qAsConst(d->m_children)) {
        if (item->isVisible() && !item->isBeingInserted()) {
            item->m_sizingInfo.percentageWithinParent = (1.0 * item->length(d->m_orientation)) / usable;
        } else {
            item->m_sizingInfo.percentageWithinParent = 0.0;
        }
    }
}

void ItemContainer::updateChildPercentages_recursive()
{
    updateChildPercentages();
    for (Item *item : qAsConst(d->m_children)) {
        if (auto c = item->asContainer())
            c->updateChildPercentages_recursive();
    }
}

QVector<double> ItemContainer::Private::childPercentages() const
{
    QVector<double> percentages;
    percentages.reserve(m_children.size());

    for (Item *item : m_children) {
        if (item->isVisible() && !item->isBeingInserted())
            percentages << item->m_sizingInfo.percentageWithinParent;
    }

    return percentages;
}

void ItemContainer::restoreChild(Item *item, NeighbourSqueezeStrategy neighbourSqueezeStrategy)
{
    Q_ASSERT(contains(item));

    const bool hadVisibleChildren = hasVisibleChildren(/*excludeBeingInserted=*/ true);

    item->setIsVisible(true);
    item->setBeingInserted(true);

    const int excessLength = d->excessLength();

    if (!hadVisibleChildren) {
        // This container was hidden and will now be restored too, since a child was restored
        if (auto c = parentContainer()) {
            setSize(item->size()); // give it a decent size. Same size as the item being restored makes sense
            c->restoreChild(this, neighbourSqueezeStrategy);
        }
    }

    // Make sure root() is big enough to respect all item's min-sizes
    updateSizeConstraints();

    item->setBeingInserted(false);

    if (numVisibleChildren() == 1) {
        // The easy case. Child is alone in the layout, occupies everything.
        item->setGeometry_recursive(rect());
        d->updateSeparators_recursive();
        return;
    }

    const int available = availableToSqueezeOnSide(item, Side1) + availableToSqueezeOnSide(item, Side2) - Item::separatorThickness;

    const int max = qMin(available, item->maxLengthHint(d->m_orientation));
    const int min = item->minLength(d->m_orientation);

    /*
     * Regarding the excessLength:
     * The layout bigger than its own max-size. The new item will get more (if it can), to counter that excess.
     * There's just 1 case where we have excess length: A layout with items with max-size, but the layout can't be smaller due to min-size constraints of the higher level layouts, in the nesting hierarchy.
     * The excess goes away when inserting a widget that can grow indefinitely, it eats all the current excess.
     */
    const int proposed = qMax(Layouting::length(item->size(), d->m_orientation), excessLength - Item::separatorThickness);
    const int newLength = qBound(min, proposed, max);

    Q_ASSERT(item->isVisible());

    // growItem() will make it grow by the same amount it steals from the neighbours, so we can't start the growing without zeroing it
    if (isVertical()) {
        item->m_sizingInfo.geometry.setHeight(0);
    } else {
        item->m_sizingInfo.geometry.setWidth(0);
    }

    growItem(item, newLength, GrowthStrategy::BothSidesEqually, neighbourSqueezeStrategy, /*accountForNewSeparator=*/ true);
    d->updateSeparators_recursive();
}

void ItemContainer::updateWidgetGeometries()
{
    for (Item *item : qAsConst(d->m_children))
        item->updateWidgetGeometries();
}

int ItemContainer::oppositeLength() const
{
    return isVertical() ? width()
                        : height();
}

void ItemContainer::requestSeparatorMove(Separator *separator, int delta)
{
    const int separatorIndex = d->m_separators.indexOf(separator);
    if (separatorIndex == -1) {
        // Doesn't happen
        qWarning() << Q_FUNC_INFO << "Unknown separator" << separator << this;
        root()->dumpLayout();
        return;
    }

    if (delta == 0)
        return;

    const int min = minPosForSeparator_global(separator);
    const int pos = separator->position();
    const int max = maxPosForSeparator_global(separator);

    if ((pos + delta < min && delta < 0) || // pos can be smaller than min, as long as we're making the distane to minPos smaller, same for max.
        (pos + delta > max && delta > 0)) { // pos can be bigger than max already and going left/up (negative delta, which is fine), just don't increase if further
        root()->dumpLayout();
        qWarning() << "Separator would have gone out of bounds"
                   << "; separators=" << separator
                   << "; min=" << min << "; pos=" << pos
                   << "; max=" << max << "; delta=" << delta;
        return;
    }

    const Side moveDirection = delta < 0 ? Side1 : Side2;
    const Item::List children = visibleChildren();
    if (children.size() <= separatorIndex) {
        // Doesn't happen
        qWarning() << Q_FUNC_INFO << "Not enough children for separator index" << separator
                   << this << separatorIndex;
        root()->dumpLayout();
        return;
    }

    int remainingToTake = qAbs(delta);
    int tookLocally = 0;

    Item *side1Neighbour = children[separatorIndex];
    Item *side2Neighbour = children[separatorIndex + 1];

    Side nextSeparatorDirection = moveDirection;

    if (moveDirection == Side1) {
        // Separator is moving left (or top if horizontal)
        const int availableSqueeze1 = availableToSqueezeOnSide(side2Neighbour, Side1);
        const int availableGrow2 = availableToGrowOnSide(side1Neighbour, Side2);

        // This is the available within our container, which we can use without bothering other separators
        tookLocally = qMin(availableSqueeze1, remainingToTake);
        tookLocally = qMin(tookLocally, availableGrow2);

        if (tookLocally != 0) {
            growItem(side2Neighbour, tookLocally, GrowthStrategy::Side1Only,
                     NeighbourSqueezeStrategy::ImmediateNeighboursFirst, false,
                     ChildrenResizeStrategy::Side1SeparatorMove);
        }

        if (availableGrow2 == tookLocally)
            nextSeparatorDirection = Side2;

    } else {

        const int availableSqueeze2 = availableToSqueezeOnSide(side1Neighbour, Side2);
        const int availableGrow1 = availableToGrowOnSide(side2Neighbour, Side1);

        // Separator is moving right (or bottom if horizontal)
        tookLocally = qMin(availableSqueeze2, remainingToTake);
        tookLocally = qMin(tookLocally, availableGrow1);

        if (tookLocally != 0) {
            growItem(side1Neighbour, tookLocally, GrowthStrategy::Side2Only,
                     NeighbourSqueezeStrategy::ImmediateNeighboursFirst, false,
                     ChildrenResizeStrategy::Side2SeparatorMove);
        }

        if (availableGrow1 == tookLocally)
            nextSeparatorDirection = Side1;
    }

    remainingToTake -= tookLocally;

    if (remainingToTake > 0) {
        // Go up the hierarchy and move the next separator on the left
        if (Q_UNLIKELY(isRoot())) {
            // Doesn't happen
            qWarning() << Q_FUNC_INFO << "Not enough space to move separator"
                       << this;
        } else {
            Separator *nextSeparator = parentContainer()->d->neighbourSeparator_recursive(this, nextSeparatorDirection, d->m_orientation);
            if (!nextSeparator) {
                // Doesn't happen
                qWarning() << Q_FUNC_INFO << "nextSeparator is null, report a bug";
                return;
            }

            // nextSeparator might not belong to parentContainer(), due to different orientation
            const int remainingDelta = moveDirection == Side1 ? -remainingToTake : remainingToTake;
            nextSeparator->parentContainer()->requestSeparatorMove(nextSeparator, remainingDelta);
        }
    }
}

void ItemContainer::requestEqualSize(Separator *separator)
{
    const int separatorIndex = d->m_separators.indexOf(separator);
    if (separatorIndex == -1) {
        // Doesn't happen
        qWarning() << Q_FUNC_INFO << "Separator not found" << separator;
        return;
    }

    const Item::List children = visibleChildren();
    Item *side1Item = children.at(separatorIndex);
    Item *side2Item = children.at(separatorIndex + 1);

    const int length1 = side1Item->length(d->m_orientation);
    const int length2 = side2Item->length(d->m_orientation);

    if (qAbs(length1 - length2) <= 1) {
        // items already have the same length, nothing to do.
        // We allow for a difference of 1px, since you can't split that.

        // But if at least 1 item is bigger than its max-size, don't bail out early, as then they don't deserve equal sizes.
        if (!(side1Item->m_sizingInfo.isPastMax(d->m_orientation) ||
              side2Item->m_sizingInfo.isPastMax(d->m_orientation))) {
            return;
        }
    }

    const int newLength = (length1 + length2) / 2;

    int delta = 0;
    if (length1 < newLength) {
        // Let's move separator to the right
        delta = newLength - length1;
    } else if (length2 < newLength) {
        // or left.
        delta = -(newLength - length2); // negative, since separator is going left
    }

    // Do some bounds checking, to respect min-size and max-size
    const int min = minPosForSeparator_global(separator, true);
    const int max = maxPosForSeparator_global(separator, true);
    const int newPos = qBound(min, separator->position() + delta, max);

    // correct the delta
    delta = newPos - separator->position();

    if (delta != 0)
        requestSeparatorMove(separator, delta);
}

void ItemContainer::layoutEqually()
{
    SizingInfo::List childSizes = sizes();
    if (!childSizes.isEmpty()) {
        layoutEqually(childSizes);
        applyGeometries(childSizes);
    }
}

void ItemContainer::layoutEqually(SizingInfo::List &sizes)
{
    const int numItems = sizes.count();
    QVector<int> satisfiedIndexes;
    satisfiedIndexes.reserve(numItems);

    int lengthToGive = length() - (d->m_separators.size() * Item::separatorThickness);

    // clear the sizes before we start distributing
    for (SizingInfo &size : sizes)
         size.setLength(0, d->m_orientation);

    while (satisfiedIndexes.count() < sizes.count()) {
        const int remainingItems = sizes.count() - satisfiedIndexes.count();
        int suggestedToGive = qMax(1, lengthToGive / remainingItems);
        const int oldLengthToGive = lengthToGive;

        for (int i = 0; i < numItems; ++i) {
            if (satisfiedIndexes.contains(i))
                continue;

            SizingInfo &size = sizes[i];
            if (size.availableToGrow(d->m_orientation) <= 0) {
                // Was already satisfied from the beginning
                satisfiedIndexes.push_back(i);
                continue;
            }

            const int newItemLenght = qBound(size.minLength(d->m_orientation),
                                             size.length(d->m_orientation) + suggestedToGive,
                                             size.maxLengthHint(d->m_orientation));
            const int toGive = newItemLenght - size.length(d->m_orientation);

            if (toGive == 0) {
                Q_ASSERT(false);
                satisfiedIndexes.push_back(i);
            } else {
                lengthToGive -= toGive;
                size.incrementLength(toGive, d->m_orientation);
                if (size.availableToGrow(d->m_orientation) <= 0) {
                    satisfiedIndexes.push_back(i);
                }
                if (lengthToGive == 0)
                    return;
            }
        }

        if (oldLengthToGive == lengthToGive) {
            // Nothing happened, we can't satisfy more items, due to min/max constraints
            return;
        }
    }
}

void ItemContainer::layoutEqually_recursive()
{
    layoutEqually();
    for (Item *item : qAsConst(d->m_children)) {
        if (item->isVisible()) {
            if (auto c = item->asContainer())
                c->layoutEqually_recursive();
        }
    }
}

Item *ItemContainer::visibleNeighbourFor(const Item *item, Side side) const
{
    // Item might not be visible, so use m_children instead of visibleChildren()
    const int index = d->m_children.indexOf(const_cast<Item*>(item));

    if (side == Side1) {
        for (int i = index - 1; i >= 0; i--) {
            Item *item = d->m_children.at(i);
            if (item->isVisible())
                return item;
        }
    } else {
        for (int i = index + 1; i < d->m_children.size(); ++i) {
            Item *item = d->m_children.at(i);
            if (item->isVisible())
                return item;
        }
    }

    return nullptr;
}

QSize ItemContainer::availableSize() const
{
    return size() - this->minSize();
}

int ItemContainer::availableLength() const
{
    return isVertical() ? availableSize().height()
                        : availableSize().width();
}

LengthOnSide ItemContainer::lengthOnSide(const SizingInfo::List &sizes, int fromIndex,
                                         Side side, Qt::Orientation o) const
{
    if (fromIndex < 0)
        return {};

    const int count = sizes.count();
    if (fromIndex >= count)
        return {};

    int start = 0;
    int end = -1;
    if (side == Side1) {
        start = 0;
        end = fromIndex;
    } else {
        start = fromIndex;
        end = count - 1;

    }

    LengthOnSide result;
    for (int i = start; i <= end; ++i) {
        const SizingInfo &size = sizes.at(i);
        result.length += size.length(o);
        result.minLength += size.minLength(o);
    }

    return result;
}

int ItemContainer::neighboursLengthFor(const Item *item, Side side, Qt::Orientation o) const
{
    const Item::List children = visibleChildren();
    const int index = children.indexOf(const_cast<Item*>(item));
    if (index == -1) {
        qWarning() << Q_FUNC_INFO << "Couldn't find item" << item;
        return 0;
    }

    if (o == d->m_orientation) {
        int neighbourLength = 0;
        int start = 0;
        int end = -1;
        if (side == Side1) {
            start = 0;
            end = index - 1;
        } else {
            start = index + 1;
            end = children.size() - 1;
        }

        for (int i = start; i <= end; ++i)
            neighbourLength += children.at(i)->length(d->m_orientation);

        return neighbourLength;
    } else {
        // No neighbours in the other orientation. Each container is bidimensional.
        return 0;
    }
}

int ItemContainer::neighboursLengthFor_recursive(const Item *item, Side side, Qt::Orientation o) const
{
    return neighboursLengthFor(item, side, o) + (isRoot() ? 0
                                                          : parentContainer()->neighboursLengthFor_recursive(this, side, o));

}

int ItemContainer::neighboursMinLengthFor(const Item *item, Side side, Qt::Orientation o) const
{
    const Item::List children = visibleChildren();
    const int index = children.indexOf(const_cast<Item*>(item));
    if (index == -1) {
        qWarning() << Q_FUNC_INFO << "Couldn't find item" << item;
        return 0;
    }

    if (o == d->m_orientation) {
        int neighbourMinLength = 0;
        int start = 0;
        int end = -1;
        if (side == Side1) {
            start = 0;
            end = index - 1;
        } else {
            start = index + 1;
            end = children.size() - 1;
        }

        for (int i = start; i <= end; ++i)
            neighbourMinLength += children.at(i)->minLength(d->m_orientation);

        return neighbourMinLength;
    } else {
        // No neighbours here
        return 0;
    }
}

int ItemContainer::neighboursMaxLengthFor(const Item *item, Side side, Qt::Orientation o) const
{
    const Item::List children = visibleChildren();
    const int index = children.indexOf(const_cast<Item*>(item));
    if (index == -1) {
        qWarning() << Q_FUNC_INFO << "Couldn't find item" << item;
        return 0;
    }

    if (o == d->m_orientation) {
        int neighbourMaxLength = 0;
        int start = 0;
        int end = -1;
        if (side == Side1) {
            start = 0;
            end = index - 1;
        } else {
            start = index + 1;
            end = children.size() - 1;
        }

        for (int i = start; i <= end; ++i)
            neighbourMaxLength = qMin(Layouting::length(root()->size(), d->m_orientation), neighbourMaxLength + children.at(i)->maxLengthHint(d->m_orientation));

         return neighbourMaxLength;
    } else {
        // No neighbours here
        return 0;
    }
}

int ItemContainer::availableToSqueezeOnSide(const Item *child, Side side) const
{
    const int length = neighboursLengthFor(child, side, d->m_orientation);
    const int min = neighboursMinLengthFor(child, side, d->m_orientation);

    const int available = length - min;
    if (available < 0) {
        root()->dumpLayout();
        Q_ASSERT(false);
    }
    return available;
}

int ItemContainer::availableToGrowOnSide(const Item *child, Side side) const
{
    const int length = neighboursLengthFor(child, side, d->m_orientation);
    const int max = neighboursMaxLengthFor(child, side, d->m_orientation);

    return max - length;
}

int ItemContainer::availableToSqueezeOnSide_recursive(const Item *child, Side side, Qt::Orientation orientation) const
{
    if (orientation == d->m_orientation) {
        const int available = availableToSqueezeOnSide(child, side);
        return isRoot() ? available
                        : (available + parentContainer()->availableToSqueezeOnSide_recursive(this, side, orientation));
    } else {
        return isRoot() ? 0
                        : parentContainer()->availableToSqueezeOnSide_recursive(this, side, orientation);
    }
}

int ItemContainer::availableToGrowOnSide_recursive(const Item *child, Side side, Qt::Orientation orientation) const
{
    if (orientation == d->m_orientation) {
        const int available = availableToGrowOnSide(child, side);
        return isRoot() ? available
                        : (available + parentContainer()->availableToGrowOnSide_recursive(this, side, orientation));
    } else {
        return isRoot() ? 0
                        : parentContainer()->availableToGrowOnSide_recursive(this, side, orientation);
    }
}

void ItemContainer::growNeighbours(Item *side1Neighbour, Item *side2Neighbour)
{
    if (!side1Neighbour && !side2Neighbour)
        return;

    SizingInfo::List childSizes = sizes();

    if (side1Neighbour && side2Neighbour) {
        const int index1 = indexOfVisibleChild(side1Neighbour);
        const int index2 = indexOfVisibleChild(side2Neighbour);

        if (index1 == -1 || index2 == -1 || index1 >= childSizes.count() || index2 >= childSizes.count()) {
            // Doesn't happen
            qWarning() << Q_FUNC_INFO << "Invalid indexes" << index1 << index2 << childSizes.count();
            return;
        }

        // Give half/half to each neighbour
        QRect &geo1 = childSizes[index1].geometry;
        QRect &geo2 = childSizes[index2].geometry;

        if (isVertical()) {
            const int available = geo2.y() - geo1.bottom() - separatorThickness;
            geo1.setHeight(geo1.height() + available / 2);
            geo2.setTop(geo1.bottom() + separatorThickness + 1);
        } else {
            const int available = geo2.x() - geo1.right() - separatorThickness;
            geo1.setWidth(geo1.width() + available / 2);
            geo2.setLeft(geo1.right() + separatorThickness + 1);
        }

    } else if (side1Neighbour) {
        const int index1 = indexOfVisibleChild(side1Neighbour);
        if (index1 == -1 || index1 >= childSizes.count()) {
            // Doesn't happen
            qWarning() << Q_FUNC_INFO << "Invalid indexes" << index1 << childSizes.count();
            return;
        }

        // Grow all the way to the right (or bottom if vertical)
        QRect &geo = childSizes[index1].geometry;
        if (isVertical()) {
            geo.setBottom(rect().bottom());
        } else {
            geo.setRight(rect().right());
        }
    } else if (side2Neighbour) {
        const int index2 = indexOfVisibleChild(side2Neighbour);
        if (index2 == -1 || index2 >= childSizes.count()) {
            // Doesn't happen
            qWarning() << Q_FUNC_INFO << "Invalid indexes" << index2 << childSizes.count();
            return;
        }

        // Grow all the way to the left (or top if vertical)
        QRect &geo = childSizes[index2].geometry;
        if (isVertical()) {
            geo.setTop(0);
        } else {
            geo.setLeft(0);
        }
    }

    d->honourMaxSizes(childSizes);
    positionItems(/*by-ref*/ childSizes);
    applyGeometries(childSizes);
}

void ItemContainer::growItem(int index, SizingInfo::List &sizes, int missing,
                             GrowthStrategy growthStrategy,
                             NeighbourSqueezeStrategy neighbourSqueezeStrategy,
                             bool accountForNewSeparator)
{
    int toSteal = missing; // The amount that neighbours of @p index will shrink
    if (accountForNewSeparator)
        toSteal += Item::separatorThickness;

    Q_ASSERT(index != -1);
    if (toSteal == 0)
        return;

    // #1. Grow our item
    SizingInfo &sizingInfo = sizes[index];
    sizingInfo.setOppositeLength(oppositeLength(), d->m_orientation);
    const bool isFirst = index == 0;
    const bool isLast = index == sizes.count() - 1;

    int side1Growth = 0;
    int side2Growth = 0;

    if (growthStrategy == GrowthStrategy::BothSidesEqually) {
        sizingInfo.setLength(sizingInfo.length(d->m_orientation) + missing, d->m_orientation);
        const int count = sizes.count();
        if (count == 1) {
            //There's no neighbours to push, we're alone. Occupy the full container
            sizingInfo.incrementLength(missing, d->m_orientation);
            return;
        }

        // #2. Now shrink the neigbours by the same amount. Calculate how much to shrink from each side
        const LengthOnSide side1Length = lengthOnSide(sizes, index - 1, Side1, d->m_orientation);
        const LengthOnSide side2Length = lengthOnSide(sizes, index + 1, Side2, d->m_orientation);

        int available1 = side1Length.available();
        int available2 = side2Length.available();

        if (toSteal > available1 + available2) {
            root()->dumpLayout();
            Q_ASSERT(false);
        }

        while (toSteal > 0) {
            if (available1 == 0) {
                Q_ASSERT(available2 >= toSteal);
                side2Growth += toSteal;
                break;
            } else if (available2 == 0) {
                Q_ASSERT(available1 >= toSteal);
                side1Growth += toSteal;
                break;
            }

            const int toTake = qMax(1, toSteal / 2);
            const int took1 = qMin(toTake, available1);
            toSteal -= took1;
            available1 -= took1;
            side1Growth += took1;
            if (toSteal == 0)
                break;

            const int took2 = qMin(toTake, available2);
            toSteal -= took2;
            side2Growth += took2;
            available2 -= took2;
        }
        shrinkNeighbours(index, sizes, side1Growth, side2Growth, neighbourSqueezeStrategy);
    } else if (growthStrategy == GrowthStrategy::Side1Only) {
        side1Growth = qMin(missing, sizingInfo.availableToGrow(d->m_orientation));
        sizingInfo.setLength(sizingInfo.length(d->m_orientation) + side1Growth, d->m_orientation);
        if (side1Growth > 0)
            shrinkNeighbours(index, sizes, side1Growth, /*side2Growth=*/ 0, neighbourSqueezeStrategy);
        if (side1Growth < missing) {
            missing = missing - side1Growth;

            if (isLast) {
                // Doesn't happen
                qWarning() << Q_FUNC_INFO << "No more items to grow";
            } else {
                growItem(index + 1, sizes, missing, growthStrategy, neighbourSqueezeStrategy, accountForNewSeparator);
            }
        }

    } else if (growthStrategy == GrowthStrategy::Side2Only) {
        side2Growth = qMin(missing, sizingInfo.availableToGrow(d->m_orientation));
        sizingInfo.setLength(sizingInfo.length(d->m_orientation) + side2Growth, d->m_orientation);

        if (side2Growth > 0)
            shrinkNeighbours(index, sizes, /*side1Growth=*/ 0, side2Growth, neighbourSqueezeStrategy);
        if (side2Growth < missing) {
            missing = missing - side2Growth;

            if (isFirst) {
                // Doesn't happen
                qWarning() << Q_FUNC_INFO << "No more items to grow";
            } else {
                growItem(index - 1, sizes, missing, growthStrategy, neighbourSqueezeStrategy, accountForNewSeparator);
            }
        }
    }
}

void ItemContainer::growItem(Item *item, int amount, GrowthStrategy growthStrategy,
                             NeighbourSqueezeStrategy neighbourSqueezeStrategy,
                             bool accountForNewSeparator,
                             ChildrenResizeStrategy childResizeStrategy)
{
    const Item::List items = visibleChildren();
    const int index = items.indexOf(item);
    SizingInfo::List sizes = this->sizes();

    growItem(index, /*by-ref=*/sizes, amount, growthStrategy, neighbourSqueezeStrategy, accountForNewSeparator);

    applyGeometries(sizes, childResizeStrategy);
}

void ItemContainer::applyGeometries(const SizingInfo::List &sizes, ChildrenResizeStrategy strategy)
{
    const Item::List items = visibleChildren();
    const int count = items.size();
    Q_ASSERT(count == sizes.size());

    for (int i = 0; i < count; ++i) {
        Item *item = items.at(i);
        item->setSize_recursive(sizes[i].geometry.size(), strategy);
    }

    positionItems();
}

SizingInfo::List ItemContainer::sizes(bool ignoreBeingInserted) const
{
    const Item::List children = visibleChildren(ignoreBeingInserted);
    SizingInfo::List result;
    result.reserve(children.count());
    for (Item *item : children) {
        if (item->isContainer()) {
            // Containers have virtual min/maxSize methods, and don't really fill in these properties
            // So fill them here
            item->m_sizingInfo.minSize = item->minSize();
            item->m_sizingInfo.maxSizeHint = item->maxSizeHint();
        }
        result << item->m_sizingInfo;
    }

    return result;
}

QVector<int> ItemContainer::calculateSqueezes(SizingInfo::List::ConstIterator begin,
                                              SizingInfo::List::ConstIterator end, int needed,
                                              NeighbourSqueezeStrategy strategy, bool reversed) const
{
    QVector<int> availabilities;
    for (auto it = begin; it < end; ++it) {
        availabilities << it->availableLength(d->m_orientation);
    }

    const int count = availabilities.count();

    QVector<int> squeezes(count, 0);
    int missing = needed;

    if (strategy == NeighbourSqueezeStrategy::AllNeighbours) {
        while (missing > 0) {
            const int numDonors = std::count_if(availabilities.cbegin(), availabilities.cend(), [] (int num) {
                return num > 0;
            });

            if (numDonors == 0) {
                root()->dumpLayout();
                Q_ASSERT(false);
                return {};
            }

            int toTake = missing / numDonors;
            if (toTake == 0)
                toTake = missing;

            for (int i = 0; i < count; ++i) {
                const int available = availabilities.at(i);
                if (available == 0)
                    continue;
                const int took = qMin(missing, qMin(toTake, available));
                availabilities[i] -= took;
                missing -= took;
                squeezes[i] += took;
                if (missing == 0)
                    break;
            }
        }
    } else if (strategy == NeighbourSqueezeStrategy::ImmediateNeighboursFirst) {
        for (int i = 0; i < count; i++) {
            const int index = reversed ? count - 1 - i : i;

            const int available = availabilities.at(index);
            if (available > 0) {
                const int took = qMin(missing, available);
                missing -= took;
                squeezes[index] += took;
            }

            if (missing == 0)
                break;
        }
    }

    if (missing < 0) {
        // Doesn't really happen
        qWarning() << Q_FUNC_INFO << "Missing is negative" << missing
                   << squeezes;
    }

    return squeezes;
}

void ItemContainer::shrinkNeighbours(int index, SizingInfo::List &sizes, int side1Amount,
                                     int side2Amount, NeighbourSqueezeStrategy strategy)
{
    Q_ASSERT(side1Amount > 0 || side2Amount > 0);
    Q_ASSERT(side1Amount >= 0 && side2Amount >= 0); // never negative

    if (side1Amount > 0) {
        auto begin = sizes.cbegin();
        auto end = sizes.cbegin() + index;
        const bool reversed = strategy == NeighbourSqueezeStrategy::ImmediateNeighboursFirst;
        const QVector<int> squeezes = calculateSqueezes(begin, end, side1Amount, strategy, reversed);
        for (int i = 0; i < squeezes.size(); ++i) {
            const int squeeze = squeezes.at(i);
            SizingInfo &sizing = sizes[i];
            // setSize() or setGeometry() have the same effect here, we don't care about the position yet. That's done in positionItems()
            sizing.setSize(adjustedRect(sizing.geometry, d->m_orientation, 0, -squeeze).size());
        }
    }

    if (side2Amount > 0) {
        auto begin = sizes.cbegin() + index + 1;
        auto end = sizes.cend();

        const QVector<int> squeezes = calculateSqueezes(begin, end, side2Amount, strategy);
        for (int i = 0; i < squeezes.size(); ++i) {
            const int squeeze = squeezes.at(i);
            SizingInfo &sizing = sizes[i + index + 1];
            sizing.setSize(adjustedRect(sizing.geometry, d->m_orientation, squeeze, 0).size());
        }
    }
}

QVector<int> ItemContainer::Private::requiredSeparatorPositions() const
{
    const int numSeparators = qMax(0, q->numVisibleChildren() - 1);
    QVector<int> positions;
    positions.reserve(numSeparators);

    for (Item *item : m_children) {
        if (positions.size() == numSeparators)
            break;

        if (item->isVisible()) {
            const int localPos = item->m_sizingInfo.edge(m_orientation) + 1;
            positions << q->mapToRoot(localPos, m_orientation);
        }
    }

    return positions;
}

void ItemContainer::Private::updateSeparators()
{
    if (!q->hostWidget())
        return;

    const QVector<int> positions = requiredSeparatorPositions();
    const int requiredNumSeparators = positions.size();

    const bool numSeparatorsChanged = requiredNumSeparators != m_separators.size();
    if (numSeparatorsChanged) {
        // Instead of just creating N missing ones at the end of the list, let's minimize separators
        // having their position changed, to minimize flicker
        Separator::List newSeparators;
        newSeparators.reserve(requiredNumSeparators);

        for (int position : positions) {
            Separator *separator = separatorAt(position);
            if (separator) {
                // Already existing, reuse
                newSeparators.push_back(separator);
                m_separators.removeOne(separator);
            } else {
                separator = Config::self().createSeparator(q->hostWidget());
                separator->init(q, m_orientation);
                newSeparators.push_back(separator);
            }
        }

        // delete what remained, which is unused
        deleteSeparators();

        m_separators = newSeparators;
    }

    // Update their positions:
    const int pos2 = q->isVertical() ? q->mapToRoot(QPoint(0, 0)).x()
                                     : q->mapToRoot(QPoint(0, 0)).y();

    int i = 0;
    for (int position : positions) {
        m_separators.at(i)->setGeometry(position, pos2, q->oppositeLength());
        i++;
    }

    q->updateChildPercentages();
}

void ItemContainer::Private::deleteSeparators()
{
    qDeleteAll(m_separators);
    m_separators.clear();
}

void ItemContainer::Private::deleteSeparators_recursive()
{
    deleteSeparators();

    // recurse into the children:
    for (Item *item : qAsConst(m_children)) {
        if (auto c = item->asContainer())
            c->d->deleteSeparators_recursive();
    }
}

void ItemContainer::Private::updateSeparators_recursive()
{
    updateSeparators();

    // recurse into the children:
    const Item::List items = q->visibleChildren();
    for (Item *item : items) {
        if (auto c = item->asContainer())
            c->d->updateSeparators_recursive();
    }
}

int ItemContainer::Private::excessLength() const
{
    // Returns how much bigger this layout is than its max-size
    return qMax(0, Layouting::length(q->size(), m_orientation) - q->maxLengthHint(m_orientation));
}

void ItemContainer::simplify()
{
    // Removes unneeded nesting. For example, a vertical layout doesn't need to have vertical layouts
    // inside. It can simply have the contents of said sub-layouts

    QScopedValueRollback<bool> isInSimplify(d->m_isSimplifying, true);

    Item::List newChildren;
    newChildren.reserve(d->m_children.size() + 20); // over-reserve a bit

    for (Item *child : qAsConst(d->m_children)) {
        if (ItemContainer *childContainer = child->asContainer()) {
            childContainer->simplify(); // recurse down the hierarchy

            if (childContainer->orientation() == d->m_orientation || childContainer->d->m_children.size() == 1) {
                // This sub-container is reduntant, as it has the same orientation as its parent
                // Canibalize it.
                for (Item *child2 : childContainer->childItems()) {
                    child2->setParentContainer(this);
                    newChildren.push_back(child2);
                }

                delete childContainer;
            } else {
                newChildren.push_back(child);
            }
        } else {
            newChildren.push_back(child);
        }
    }

    if (d->m_children != newChildren) {
        d->m_children = newChildren;
        positionItems();
        updateChildPercentages();
    }
}

Separator *ItemContainer::Private::separatorAt(int p) const
{
    for (Separator *separator : m_separators) {
        if (separator->position() == p)
            return separator;
    }

    return nullptr;
}

bool ItemContainer::isVertical() const
{
    return d->m_orientation == Qt::Vertical;
}

bool ItemContainer::isHorizontal() const
{
    return d->m_orientation == Qt::Horizontal;
}

int ItemContainer::indexOf(Separator *separator) const
{
    return d->m_separators.indexOf(separator);
}

bool ItemContainer::isInSimplify() const
{
    if (d->m_isSimplifying)
        return true;

    auto p = parentContainer();
    return p && p->isInSimplify();
}

int ItemContainer::minPosForSeparator(Separator *separator, bool honourMax) const
{
    const int globalMin = minPosForSeparator_global(separator, honourMax);
    return mapFromRoot(globalMin, d->m_orientation);
}

int ItemContainer::maxPosForSeparator(Separator *separator, bool honourMax) const
{
    const int globalMax = maxPosForSeparator_global(separator, honourMax);
    return mapFromRoot(globalMax, d->m_orientation);
}

int ItemContainer::minPosForSeparator_global(Separator *separator, bool honourMax) const
{
    const int separatorIndex = indexOf(separator);
    Q_ASSERT(separatorIndex != -1);

    const Item::List children = visibleChildren();
    Q_ASSERT(separatorIndex + 1 < children.size());
    Item *item2 = children.at(separatorIndex + 1);

    const int availableToSqueeze = availableToSqueezeOnSide_recursive(item2, Side1, d->m_orientation);

    if (honourMax) {
        // We can drag the separator left just as much as it doesn't violate max-size constraints of Side2
        Item *item1 = children.at(separatorIndex);
        const int availabletoGrow = availableToGrowOnSide_recursive(item1, Side2, d->m_orientation);
        return separator->position() - qMin(availabletoGrow, availableToSqueeze);
    }

    return separator->position() - availableToSqueeze;
}

int ItemContainer::maxPosForSeparator_global(Separator *separator, bool honourMax) const
{
    const int separatorIndex = indexOf(separator);
    Q_ASSERT(separatorIndex != -1);

    const Item::List children = visibleChildren();
    Item *item1 = children.at(separatorIndex);

    const int availableToSqueeze = availableToSqueezeOnSide_recursive(item1, Side2, d->m_orientation);

    if (honourMax) {
        // We can drag the separator right just as much as it doesn't violate max-size constraints of Side1
        Item *item2 = children.at(separatorIndex + 1);
        const int availabletoGrow = availableToGrowOnSide_recursive(item2, Side1, d->m_orientation);
        return separator->position() + qMin(availabletoGrow, availableToSqueeze);
    }

    return separator->position() + availableToSqueeze;
}

QVariantMap ItemContainer::toVariantMap() const
{
    QVariantMap result = Item::toVariantMap();

    QVariantList childrenV;
    childrenV.reserve(d->m_children.size());
    for (Item *child : qAsConst(d->m_children)) {
        childrenV.push_back(child->toVariantMap());
    }

    result[QStringLiteral("children")] = childrenV;
    result[QStringLiteral("orientation")] = d->m_orientation;

    return result;
}

void ItemContainer::fillFromVariantMap(const QVariantMap &map,
                                       const QHash<QString, Widget*> &widgets)
{
    QScopedValueRollback<bool> deserializing(d->m_isDeserializing, true);

    Item::fillFromVariantMap(map, widgets);
    const QVariantList childrenV = map[QStringLiteral("children")].toList();
    d->m_orientation = Qt::Orientation(map[QStringLiteral("orientation")].toInt());

    for (const QVariant &childV : childrenV) {
        const QVariantMap childMap = childV.toMap();
        const bool isContainer = childMap[QStringLiteral("isContainer")].toBool();
        Item *child = isContainer ? new ItemContainer(hostWidget(), this)
                                  : new Item(hostWidget(), this);
        child->fillFromVariantMap(childMap, widgets);
        d->m_children.push_back(child);
    }

    if (isRoot()) {
        updateChildPercentages_recursive();
        if (hostWidget()) {
            d->updateSeparators_recursive();
            d->updateWidgets_recursive();
        }

        d->relayoutIfNeeded();
        positionItems_recursive();

        Q_EMIT minSizeChanged(this);
#ifdef DOCKS_DEVELOPER_MODE
    if (!checkSanity())
        qWarning() << Q_FUNC_INFO << "Resulting layout is invalid";
#endif
    }
}

bool ItemContainer::Private::isDummy() const
{
    return q->hostWidget() == nullptr;
}

#ifdef DOCKS_DEVELOPER_MODE
bool ItemContainer::test_suggestedRect()
{
    auto itemToDrop = new Item(hostWidget());

    const Item::List children = visibleChildren();
    for (Item *relativeTo : children) {
        if (auto c = relativeTo->asContainer()) {
            c->test_suggestedRect();
        } else {
            QHash<Location, QRect> rects;
            for (Location loc : { Location_OnTop, Location_OnLeft, Location_OnRight, Location_OnBottom}) {
                const QRect rect = suggestedDropRect(itemToDrop, relativeTo, loc);
                rects.insert(loc, rect);
                if (rect.isEmpty()) {
                    qWarning() << Q_FUNC_INFO << "Empty rect";
                    return false;
                } else if (!root()->rect().contains(rect)) {
                    root()->dumpLayout();
                    qWarning() << Q_FUNC_INFO << "Suggested rect is out of bounds" << rect
                               << "; loc=" << loc << "; relativeTo=" << relativeTo;
                    return false;
                }
            }
            if (rects.value(Location_OnBottom).y() <= rects.value(Location_OnTop).y() ||
                rects.value(Location_OnRight).x() <= rects.value(Location_OnLeft).x()) {
                root()->dumpLayout();
                qWarning() << Q_FUNC_INFO << "Invalid suggested rects" << rects
                           << this << "; relativeTo=" << relativeTo;
                return false;
            }
        }
    }

    delete itemToDrop;
    return true;
}
#endif

QVector<Separator *> ItemContainer::separators_recursive() const
{
    Layouting::Separator::List separators = d->m_separators;

    for (Item *item : qAsConst(d->m_children)) {
        if (auto c = item->asContainer())
            separators << c->separators_recursive();
    }

    return separators;
}

QVector<Separator *> ItemContainer::separators() const
{
    return d->m_separators;
}

bool ItemContainer::Private::isOverflowing() const
{
    // This never returns true, unless when loading a buggy layout
    // or if QWidgets now have bigger min-size

    int contentsLength = 0;
    int numVisible = 0;
    for (Item *item : m_children) {
        if (item->isVisible()) {
            contentsLength += item->length(m_orientation);
            numVisible++;
        }
    }

    contentsLength += qMax(0, Item::separatorThickness * (numVisible - 1));
    return contentsLength > q->length();
}

void ItemContainer::Private::relayoutIfNeeded()
{
    // Checks all the child containers if they have the correct min-size, recursively.
    // When loading a layout from disk the min-sizes for the host QWidgets might have changed, so we
    // need to adjust

    if (!q->missingSize().isNull())
        q->setSize_recursive(q->minSize());

    if (isOverflowing()) {
        const QSize size = q->size();
        q->m_sizingInfo.setSize(size + QSize(1, 1)); // Just so setSize_recursive() doesn't bail out
        q->setSize_recursive(size);
        q->updateChildPercentages();
    }

    // Let's see our children too:
    for (Item *item : qAsConst(m_children)) {
        if (item->isVisible()) {
            if (auto c = item->asContainer())
                c->d->relayoutIfNeeded();
        }
    }

}

const Item *ItemContainer::Private::itemFromPath(const QVector<int> &path) const
{
    const ItemContainer *container = q;

    for (int i = 0; i < path.size() ; ++i) {
        const int index = path[i];
        const bool isLast = i == path.size() - 1;
        if (index < 0 || index >= container->d->m_children.size()) {
            // Doesn't happen
            q->root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Invalid index" << index
                       << this << path << q->isRoot();
            return nullptr;
        }

        if (isLast) {
            return container->d->m_children.at(index);
        } else {
            container = container->d->m_children.at(index)->asContainer();
            if (!container) {
                qWarning() << Q_FUNC_INFO << "Invalid index" << path;
                return nullptr;
            }
        }
    }

    return q;
}

Separator *ItemContainer::Private::neighbourSeparator(const Item *item, Side side, Qt::Orientation orientation) const
{
    Item::List children = q->visibleChildren();
    const int itemIndex = children.indexOf(const_cast<Item *>(item));
    if (itemIndex == -1) {
        qWarning() << Q_FUNC_INFO << "Item not found" << item
                   << this;
        q->root()->dumpLayout();
        return nullptr;
    }

    if (orientation != q->orientation()) {
        // Go up
        if (q->isRoot()) {
            return nullptr;
        } else {
            return q->parentContainer()->d->neighbourSeparator(q, side, orientation);
        }
    }

    const int separatorIndex = side == Side1 ? itemIndex -1
                                             : itemIndex;

    if (separatorIndex < 0 || separatorIndex >= m_separators.size())
        return nullptr;

    return m_separators[separatorIndex];
}

Separator *ItemContainer::Private::neighbourSeparator_recursive(const Item *item, Side side,
                                                                Qt::Orientation orientation) const
{
    Separator *separator = neighbourSeparator(item, side, orientation);
    if (separator)
        return separator;

    if (!q->parentContainer())
        return nullptr;

    return q->parentContainer()->d->neighbourSeparator_recursive(q, side, orientation);
}

void ItemContainer::Private::updateWidgets_recursive()
{
    for (Item *item : qAsConst(m_children)) {
        if (auto c = item->asContainer()) {
            c->d->updateWidgets_recursive();
        } else {
            if (item->isVisible()) {
                if (Widget *widget = item->guestWidget()) {
                    widget->setGeometry(q->mapToRoot(item->geometry()));
                    widget->setVisible(true);
                } else {
                    qWarning() << Q_FUNC_INFO << "visible item doesn't have a guest"
                               << item;
                }
            }
        }
    }
}

void SizingInfo::setOppositeLength(int l, Qt::Orientation o)
{
    setLength(l, oppositeOrientation(o));
}

QVariantMap SizingInfo::toVariantMap() const
{
    QVariantMap result;
    result[QStringLiteral("geometry")] = rectToMap(geometry);
    result[QStringLiteral("minSize")] = sizeToMap(minSize);
    result[QStringLiteral("maxSize")] = sizeToMap(maxSizeHint);
    return result;
}

void SizingInfo::fromVariantMap(const QVariantMap &map)
{
    *this = SizingInfo(); // reset any non-important fields to their default
    geometry = mapToRect(map[QStringLiteral("geometry")].toMap());
    minSize = mapToSize(map[QStringLiteral("minSize")].toMap());
    maxSizeHint = mapToSize(map[QStringLiteral("maxSize")].toMap());
}

int ItemContainer::Private::defaultLengthFor(Item *item, DefaultSizeMode mode) const
{
    int result = 0;
    switch (mode) {
    case DefaultSizeMode::None:
        break;
    case DefaultSizeMode::Fair: {
        const int numVisibleChildren = q->numVisibleChildren() + 1; // +1 so it counts with @p item too, which we're adding
        const int usableLength = q->length() - (Item::separatorThickness*(numVisibleChildren - 1));
        result = usableLength / numVisibleChildren;
        break;
    }
    case DefaultSizeMode::FairButFloor: {
        int length = defaultLengthFor(item, DefaultSizeMode::Fair);
        result = qMin(length, item->length(m_orientation));
        break;
    }
    case DefaultSizeMode::ItemSize:
        result = item->length(m_orientation);
        break;
    case DefaultSizeMode::SizePolicy:
        qWarning() << Q_FUNC_INFO << "Now implemented yet";
        break;
    }

    result = qMax(item->minLength(m_orientation), result); // bound with max-size too
    return result;
}

#ifdef Q_CC_MSVC
# pragma warning(pop)
#endif
