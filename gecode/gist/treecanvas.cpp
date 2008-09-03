/*
 *  Main authors:
 *     Guido Tack <tack@gecode.org>
 *
 *  Copyright:
 *     Guido Tack, 2006
 *
 *  Last modified:
 *     $Date$ by $Author$
 *     $Revision$
 *
 *  This file is part of Gecode, the generic constraint
 *  development environment:
 *     http://www.gecode.org
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <QtGui/QPainter>

#include <stack>
#include <fstream>

#include "gecode/gist/treecanvas.hh"

#include "gecode/gist/nodevisitor.hh"
#include "gecode/gist/layoutcursor.hh"
#include "gecode/gist/visualnode.hh"
#include "gecode/gist/drawingcursor.hh"
#include "gecode/gist/addchild.hh"
#include "gecode/gist/addvisualisationdialog.hh"
#include "gecode/gist/zoomToFitIcon.hpp"

#include "gecode/search.hh"

namespace Gecode { namespace Gist {

  const int minScale = 10;
  const int maxScale = 400;
  const int defScale = 100;
  const int maxAutoZoomScale = defScale;

  Inspector::~Inspector(void) {}
    
  TreeCanvasImpl::TreeCanvasImpl(Space* rootSpace, bool bab,
                                 QWidget* parent)
    : QWidget(parent)
    , mutex(QMutex::Recursive)
    , layoutMutex(QMutex::Recursive)
    , inspector(NULL)
    , autoHideFailed(true), autoZoom(false)
    , refresh(500), smoothScrollAndZoom(false), nextPit(0)
    , targetZoom(defScale), metaZoomCurrent(static_cast<double>(defScale))
    , zoomTimerId(0)
    , targetScrollX(0), targetScrollY(0)
    , metaScrollXCurrent(0.0), metaScrollYCurrent(0.0)
    , scrollTimerId(0) {
      QMutexLocker locker(&mutex);
      curBest = (bab ? new BestNode(NULL) : NULL);
      na = new Node::NodeAllocator();
      root = new (*na) VisualNode(rootSpace);
      root->layout();
      root->setMarked(true);
      currentNode = root;
      pathHead = root;
      scale = defScale / 100.0;

      setAutoFillBackground(true);

      connect(&searcher, SIGNAL(update(int,int,int)), this, 
                         SLOT(layoutDone(int,int,int)));
      connect(&searcher, SIGNAL(statusChanged(bool)), this, 
              SLOT(statusChanged(bool)));
      
      qRegisterMetaType<Statistics>("Statistics");
      update();
  }
  
  TreeCanvasImpl::~TreeCanvasImpl(void) { delete root; delete na; }

  void
  TreeCanvasImpl::setInspector(Inspector* i) { inspector = i; }

  void
  TreeCanvasImpl::scaleTree(int scale0) {
    QMutexLocker locker(&layoutMutex);
    BoundingBox bb;
    scale0 = std::min(std::max(scale0, minScale), maxScale);
    scale = (static_cast<double>(scale0)) / 100.0;
    bb = root->getBoundingBox();
    int w = 
      static_cast<int>((bb.right-bb.left+Layout::extent)*scale);
    int h = 
      static_cast<int>(2*Layout::extent+root->depth()*Layout::dist_y*scale);
    resize(w,h);
    emit scaleChanged(scale0);
    QWidget::update();
  }

  void
  TreeCanvasImpl::update(void) {
    QMutexLocker locker(&mutex);
    layoutMutex.lock();
    if (root != NULL) {
      root->layout();
      BoundingBox bb = root->getBoundingBox();

      int w = static_cast<int>((bb.right-bb.left+Layout::extent)*scale);
      int h = 
        static_cast<int>(2*Layout::extent+root->depth()*Layout::dist_y*scale);
      xtrans = -bb.left+(Layout::extent / 2);
      resize(w,h);
    }
    if (autoZoom)
      zoomToFit();
    layoutMutex.unlock();
    QWidget::update();
  }

  void
  TreeCanvasImpl::layoutDone(int w, int h, int scale0) {
    if (!smoothScrollAndZoom) {
      scaleTree(scale0);
    } else {
      metaZoomCurrent = static_cast<int>(scale*100);
      targetZoom = scale0;
      targetZoom = std::min(std::max(targetZoom, minScale), 
                            maxAutoZoomScale);
      zoomTimerId = startTimer(15);
    }
    resize(w,h);
    QWidget::update();
  }

  void
  TreeCanvasImpl::statusChanged(bool finished) {
    if (finished)
      centerCurrentNode();
    emit statusChanged(currentNode, stats, finished);
  }

  void
  SearcherThread::search(VisualNode* n, bool all, TreeCanvasImpl* ti) {
    node = n;
    a = all;
    t = ti;
    start();
  }
  
  void
  SearcherThread::updateCanvas(void) {
    t->layoutMutex.lock();
    if (t->root == NULL)
      return;

    if (t->autoHideFailed) {
      t->root->hideFailed();
    }
    t->root->layout();
    BoundingBox bb = t->root->getBoundingBox();

    int w = static_cast<int>((bb.right-bb.left+Layout::extent)*t->scale);
    int h = static_cast<int>(2*Layout::extent+
                             t->root->depth()*Layout::dist_y*t->scale);
    t->xtrans = -bb.left+(Layout::extent / 2);

    int scale0 = static_cast<int>(t->scale*100);
    if (t->autoZoom) {
      QWidget* p = t->parentWidget();
      if (p) {
        double newXScale =
          static_cast<double>(p->width()) / (bb.right - bb.left + 
                                             Layout::extent);
        double newYScale =
          static_cast<double>(p->height()) /
          (t->root->depth() * Layout::dist_y + 2*Layout::extent);

        scale0 = static_cast<int>(std::min(newXScale, newYScale)*100);
        if (scale0<minScale)
          scale0 = minScale;
        if (scale0>maxAutoZoomScale)
          scale0 = maxAutoZoomScale;
        double scale = (static_cast<double>(scale0)) / 100.0;

        w = static_cast<int>((bb.right-bb.left+Layout::extent)*scale);
        h = static_cast<int>(2*Layout::extent+
                             t->root->depth()*Layout::dist_y*scale);
      }
    }
    t->layoutMutex.unlock();
    emit update(w,h,scale0);
  }
  
  void
  SearcherThread::run() {
    {
      t->mutex.lock();
      emit statusChanged(false);
      std::stack<VisualNode*> stck;
      stck.push(node);

      VisualNode* sol = NULL;
      int nodeCount = 0;
      t->stopSearchFlag = false;
      while (!stck.empty() && !t->stopSearchFlag) {
        if (t->refresh > 0 && ++nodeCount > t->refresh) {
          node->dirtyUp();
          updateCanvas();
          emit statusChanged(false);
          nodeCount = 0;
        }
        VisualNode* n = stck.top(); stck.pop();
        if (n->isOpen()) {
          int kids = n->getNumberOfChildNodes(*t->na, t->curBest, t->stats);
          if (!a && n->getStatus() == SOLVED) {
            sol = n;
            break;
          }
          for (int i=kids; i--;) {
            stck.push(n->getChild(i));
          }
        }
      }
      node->dirtyUp();
      t->stopSearchFlag = false;
      t->mutex.unlock();
      if (sol != NULL) {
        t->setCurrentNode(sol);
      } else {
        t->setCurrentNode(node);
      }
    }
    updateCanvas();
    emit statusChanged(true);
  }

  void
  TreeCanvasImpl::searchAll(void) {
    QMutexLocker locker(&mutex);
    searcher.search(currentNode, true, this);
  }

  void
  TreeCanvasImpl::searchOne(void) {
    QMutexLocker locker(&mutex);
    searcher.search(currentNode, false, this);
  }

  void
  TreeCanvasImpl::toggleHidden(void) {
    QMutexLocker locker(&mutex);
    currentNode->toggleHidden();
    update();
    centerCurrentNode();
  }
  
  void
  TreeCanvasImpl::hideFailed(void) {
    QMutexLocker locker(&mutex);
    currentNode->hideFailed();
    update();
    centerCurrentNode();
  }
  
  void
  TreeCanvasImpl::unhideAll(void) {
    QMutexLocker locker(&mutex);
    QMutexLocker layoutLocker(&layoutMutex);
    currentNode->unhideAll();
    update();
    centerCurrentNode();
  }

  void
  TreeCanvasImpl::timerEvent(QTimerEvent* e) {
    if (e->timerId() == zoomTimerId) {
      double offset = static_cast<double>(targetZoom - metaZoomCurrent) / 6.0;
      metaZoomCurrent += offset;
      scaleBar->setValue(static_cast<int>(metaZoomCurrent));
      if (static_cast<int>(metaZoomCurrent+.5) == targetZoom) {
        killTimer(zoomTimerId);
        zoomTimerId = 0;
      }
    } else if (e->timerId() == scrollTimerId) {
      QScrollArea* sa = 
        static_cast<QScrollArea*>(parentWidget()->parentWidget());

      double xoffset =
        static_cast<double>(targetScrollX - metaScrollXCurrent) / 6.0;
      metaScrollXCurrent += xoffset;
      sa->horizontalScrollBar()
        ->setValue(static_cast<int>(metaScrollXCurrent));
      double yoffset =
        static_cast<double>(targetScrollY - metaScrollYCurrent) / 6.0;
      metaScrollYCurrent += yoffset;
      sa->verticalScrollBar()
        ->setValue(static_cast<int>(metaScrollYCurrent));

      if (static_cast<int>(metaScrollXCurrent+.5) == targetScrollX &&
          static_cast<int>(metaScrollYCurrent+.5) == targetScrollY) {
        killTimer(scrollTimerId);
        scrollTimerId = 0;
      }      
    }
  }
  
  void
  TreeCanvasImpl::zoomToFit(void) {
    QMutexLocker locker(&layoutMutex);
    if (root != NULL) {
      BoundingBox bb;
      bb = root->getBoundingBox();
      QWidget* p = parentWidget();
      if (p) {
        double newXScale =
          static_cast<double>(p->width()) / (bb.right - bb.left + 
                                             Layout::extent);
        double newYScale =
          static_cast<double>(p->height()) / (root->depth() * Layout::dist_y +
                                              2*Layout::extent);
        int scale0 = static_cast<int>(std::min(newXScale, newYScale)*100);
        if (scale0<minScale)
          scale0 = minScale;
        if (scale0>maxAutoZoomScale)
          scale0 = maxAutoZoomScale;

        if (!smoothScrollAndZoom) {
          scaleTree(scale0);
        } else {
          metaZoomCurrent = static_cast<int>(scale*100);
          targetZoom = scale0;
          targetZoom = std::min(std::max(targetZoom, minScale), 
                                maxAutoZoomScale);
          zoomTimerId = startTimer(15);
        }
      }
    }
  }

  void
  TreeCanvasImpl::centerCurrentNode(void) {
    QMutexLocker locker(&mutex);
    int x=0;
    int y=0;

    VisualNode* c = currentNode;
    while (c != NULL) {
      x += c->getOffset();
      y += Layout::dist_y;
      c = c->getParent();
    }
    
    x = static_cast<int>((xtrans+x)*scale); y = static_cast<int>(y*scale);

    QScrollArea* sa = 
      static_cast<QScrollArea*>(parentWidget()->parentWidget());

    if (!smoothScrollAndZoom) {
      sa->ensureVisible(x,y);
    } else {
      x -= sa->viewport()->width() / 2;
      y -= sa->viewport()->height() / 2;

      metaScrollXCurrent = sa->horizontalScrollBar()->value();
      metaScrollYCurrent = sa->verticalScrollBar()->value();
      targetScrollX = std::max(sa->horizontalScrollBar()->minimum(), x);
      targetScrollX = std::min(sa->horizontalScrollBar()->maximum(), 
                               targetScrollX);
      targetScrollY = std::max(sa->verticalScrollBar()->minimum(), y);
      targetScrollY = std::min(sa->verticalScrollBar()->maximum(), 
                               targetScrollY);
      scrollTimerId = startTimer(15);
    }
  }

  class SizeCursor : public NodeCursor<VisualNode> {
  public:
    size_t s;
    SizeCursor(VisualNode* theNode) : NodeCursor<VisualNode>(theNode), s(0) {}
    void processCurrentNode(void) {
      size_t ns = node()->size();
      // std::cout << "p " << ns << " children " << node()->getNumberOfChildren() << std::endl;
      s += ns;
    }
  };

  void
  TreeCanvasImpl::inspectCurrentNode(void) {
    QMutexLocker locker(&mutex);
    
    if (currentNode->isHidden()) {
      toggleHidden();
      return;
    }
    
    switch (currentNode->getStatus()) {
    case UNDETERMINED:
        {
          (void) currentNode->getNumberOfChildNodes(*na, curBest,stats);
          emit statusChanged(currentNode,stats,true);
        }
        break;
    case FAILED:
    case STEP:
    case SPECIAL:
    case BRANCH:
    case DECOMPOSE:
    case COMPONENT_IGNORED:
    case SINGLETON:
    case SOLVED:
      {
        // SizeCursor sc(currentNode);
        // PreorderNodeVisitor<SizeCursor> pnv(sc);
        // int nodes = 1;
        // while (pnv.next()) { nodes++; }
        // std::cout << "sizeof(VisualNode): " << sizeof(VisualNode)
        //           << std::endl;
        // std::cout << "Size: " << (pnv.getCursor().s)/1024 << std::endl;
        // std::cout << "Nodes: " << nodes << std::endl;
        // std::cout << "Size / node: " << (pnv.getCursor().s)/nodes
        //           << std::endl;
    
        Space* curSpace = currentNode->getSpace(curBest);
        if (currentNode->getStatus() == SOLVED &&
            curSpace->status() != SS_SOLVED) {
          // in the presence of weakly monotonic propagators, we may have to
          // use search to find the solution here
          Space* dfsSpace = dfs(curSpace);
          delete curSpace;
          curSpace = dfsSpace;
        }
        Reflection::VarMap vm;
        curSpace->getVars(vm, false);
        emit inspect(vm, nextPit);
        saveCurrentNode();
        if (inspector != NULL) {
          inspector->inspect(*curSpace);
        }
        delete curSpace;
      }
      break;
    }
    
    currentNode->dirtyUp();
    update();
    centerCurrentNode();
  }

  void
  TreeCanvasImpl::stopSearch(void) {
    stopSearchFlag = true;
  }

  void
  TreeCanvasImpl::reset(void) {
    QMutexLocker locker(&mutex);
    Space* rootSpace = root->getSpace(curBest);
    if (curBest != NULL) {
      delete curBest;
      curBest = new BestNode(NULL);
    }
    delete root;
    delete na;
    na = new Node::NodeAllocator();
    root = new (*na) VisualNode(rootSpace);
    root->setMarked(true);
    currentNode = root;
    pathHead = root;
    nodeMap.clear();
    nextPit = 0;
    scale = 1.0;
    stats = Statistics();
    root->layout();
    
    emit statusChanged(currentNode, stats, true);
    update();
  }

  void
  TreeCanvasImpl::setPath(void) {
    QMutexLocker locker(&mutex);
    if(currentNode == pathHead)
      return;

    pathHead->unPathUp();
    pathHead = currentNode;

    currentNode->pathUp();
    currentNode->dirtyUp();
    update();
  }

  void
  TreeCanvasImpl::inspectPath(void) {
    QMutexLocker locker(&mutex);
    setCurrentNode(root);
    if (currentNode->isOnPath()) {
      inspectCurrentNode();
      int nextAlt = currentNode->getPathAlternative();
      while (nextAlt >= 0) {
        setCurrentNode(currentNode->getChild(nextAlt));
        inspectCurrentNode();
        nextAlt = currentNode->getPathAlternative();
      }
    }
    update();
  }

  void
  TreeCanvasImpl::navUp(void) {
    QMutexLocker locker(&mutex);
    
    VisualNode* p = currentNode->getParent();
    
    setCurrentNode(p);

    if (p != NULL) {
      centerCurrentNode();
    }
  }

  void
  TreeCanvasImpl::navDown(void) {
    QMutexLocker locker(&mutex);
    if (!currentNode->isHidden()) {
      switch (currentNode->getStatus()) {
      case STEP:
      case SPECIAL:
        if (currentNode->getNumberOfChildren() < 1)
          break;
      case COMPONENT_IGNORED:
      case DECOMPOSE:
      case BRANCH: 
          {
            int alt = std::max(0, currentNode->getPathAlternative());
            VisualNode* n = currentNode->getChild(alt);
            setCurrentNode(n);
            centerCurrentNode();
            break;
          }
      case SINGLETON:
      case SOLVED:
      case FAILED:
      case UNDETERMINED:
        break;
      }
    }
  }

  void
  TreeCanvasImpl::navLeft(void) {
    QMutexLocker locker(&mutex);
    VisualNode* p = currentNode->getParent();
    if (p != NULL) {
      int alt = currentNode->getAlternative();
      if (alt > 0) {
        VisualNode* n = p->getChild(alt-1);
        setCurrentNode(n);
        centerCurrentNode();
      }
    }
  }

  void
  TreeCanvasImpl::navRight(void) {
    QMutexLocker locker(&mutex);
    VisualNode* p = currentNode->getParent();
    if (p != NULL) {
      int alt = currentNode->getAlternative();
      if (alt + 1 < p->getNumberOfChildNodes(*na, curBest)) {
        VisualNode* n = p->getChild(alt+1);
        setCurrentNode(n);
        centerCurrentNode();
      }
    }
  }
  
  void
  TreeCanvasImpl::navRoot(void) {
    QMutexLocker locker(&mutex);
    setCurrentNode(root);
    centerCurrentNode();
  }

  void
  TreeCanvasImpl::navNextSol(bool back) {
    QMutexLocker locker(&mutex);
    NextSolCursor nsc(currentNode, back);
    PreorderNodeVisitor<NextSolCursor> nsv(nsc);
    while (nsv.next()) {}
    if (nsv.getCursor().node() != root) {
      setCurrentNode(nsv.getCursor().node());
      centerCurrentNode();
    }
  }

  void
  TreeCanvasImpl::navPrevSol(void) {
    navNextSol(true);
  }
  
  void
  TreeCanvasImpl::markCurrentNode(int pit) {
    QMutexLocker locker(&mutex);
    if(nodeMap.size() > pit && nodeMap[pit] != NULL) {
      setCurrentNode(nodeMap[pit]);
      centerCurrentNode();
      emit pointInTimeChanged(pit);
    }
  }

  void
  TreeCanvasImpl::saveCurrentNode(void) {
    QMutexLocker locker(&mutex);
    assert(nextPit == nodeMap.size());
    nodeMap << currentNode;
    nextPit++;
  }

  void
  TreeCanvasImpl::exportNodePDF(VisualNode* n) {
#if QT_VERSION >= 0x040400
    QString filename = QFileDialog::getSaveFileName(this, tr("Export tree as pdf"), "", tr("PDF (*.pdf)"));
    if (filename != "") {
      QPrinter printer(QPrinter::ScreenResolution);
      QMutexLocker locker(&mutex);

      BoundingBox bb = n->getBoundingBox();
      printer.setFullPage(true);
      printer.setPaperSize(QSizeF(bb.right-bb.left+Layout::extent,
                                  n->depth() * Layout::dist_y + 
                                  Layout::extent), QPrinter::Point);
      printer.setOutputFileName(filename);
      QPainter painter(&printer);

      painter.setRenderHint(QPainter::Antialiasing);

      QRect pageRect = printer.pageRect();
      double newXScale =
        static_cast<double>(pageRect.width()) / (bb.right - bb.left + 
                                                 Layout::extent);
      double newYScale =
        static_cast<double>(pageRect.height()) /
                            (n->depth() * Layout::dist_y + 
                             Layout::extent);
      double printScale = std::min(newXScale, newYScale);
      painter.scale(printScale,printScale);

      int printxtrans = -bb.left+(Layout::extent / 2);

      painter.translate(printxtrans, Layout::dist_y / 2);
      QRect clip(0,0,0,0);
      DrawingCursor dc(n, curBest, painter, clip);
      currentNode->setMarked(false);
      PreorderNodeVisitor<DrawingCursor> v(dc);
      while (v.next()) {}
      currentNode->setMarked(true);
    }
#endif
  }

  void
  TreeCanvasImpl::exportWholeTreePDF(void) {
#if QT_VERSION >= 0x040400
    exportNodePDF(root);
#endif
  }

  void
  TreeCanvasImpl::exportPDF(void) {
#if QT_VERSION >= 0x040400
    exportNodePDF(currentNode);
#endif
  }

  void
  TreeCanvasImpl::print(void) {
    QPrinter printer;
    if (QPrintDialog(&printer, this).exec() == QDialog::Accepted) {
      QMutexLocker locker(&mutex);

      BoundingBox bb = root->getBoundingBox();
      QRect pageRect = printer.pageRect();
      double newXScale =
        static_cast<double>(pageRect.width()) / (bb.right - bb.left + 
                                                 Layout::extent);
      double newYScale =
        static_cast<double>(pageRect.height()) /
                            (root->depth() * Layout::dist_y + 
                             2*Layout::extent);
      double printScale = std::min(newXScale, newYScale)*100;
      if (printScale<1.0)
        printScale = 1.0;
      if (printScale > 400.0)
        printScale = 400.0;
      printScale = printScale / 100.0;

      QPainter painter(&printer);
      painter.setRenderHint(QPainter::Antialiasing);
      painter.scale(printScale,printScale);
      painter.translate(xtrans, 0);
      QRect clip(0,0,0,0);
      DrawingCursor dc(root, curBest, painter, clip);
      PreorderNodeVisitor<DrawingCursor> v(dc);
      while (v.next()) {}
    }
  }

  VisualNode*
  TreeCanvasImpl::eventNode(QEvent* event) {
    int x = 0;
    int y = 0;
    switch (event->type()) {
    case QEvent::ToolTip:
        {
          QHelpEvent* he = static_cast<QHelpEvent*>(event);
          x = he->x();
          y = he->y();
          break;
        }
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
        {
          QMouseEvent* me = static_cast<QMouseEvent*>(event);
          x = me->x();
          y = me->y();
          break;
        }
    case QEvent::ContextMenu:
        {
          QContextMenuEvent* ce = static_cast<QContextMenuEvent*>(event);
          x = ce->x();
          y = ce->y();
          break;
        }
    default:
      return NULL;
    }
    VisualNode* n;
    n = root->findNode(static_cast<int>(x/scale-xtrans),
                       static_cast<int>((y-30)/scale));
    return n;  
  }
  
  bool
  TreeCanvasImpl::event(QEvent* event) {
    if (mutex.tryLock()) {
      if (event->type() == QEvent::ToolTip) {
        VisualNode* n = eventNode(event);
        if (n != NULL && !n->isHidden() &&
            (n->getStatus() == BRANCH || n->getStatus() == DECOMPOSE)) {
          QHelpEvent* he = static_cast<QHelpEvent*>(event);
          QToolTip::showText(he->globalPos(), 
                             QString(n->toolTip(curBest).c_str()));
        } else {
          QToolTip::hideText();
        }
      }
      mutex.unlock();
    }
    return QWidget::event(event);
  }
  
  void
  TreeCanvasImpl::resizeToOuter(void) {
    if (autoZoom)
      zoomToFit();
  }
  
  void
  TreeCanvasImpl::paintEvent(QPaintEvent* event) {
    QMutexLocker locker(&layoutMutex);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    BoundingBox bb = root->getBoundingBox();
    QRect origClip = event->rect();
    painter.translate(0, 30);
    painter.scale(scale,scale);
    painter.translate(xtrans, 0);
    QRect clip(static_cast<int>(origClip.x()/scale-xtrans), 
               static_cast<int>(origClip.y()/scale),
               static_cast<int>(origClip.width()/scale), 
               static_cast<int>(origClip.height()/scale));
    DrawingCursor dc(root, curBest, painter, clip);
    PreorderNodeVisitor<DrawingCursor> v(dc);

    while (v.next()) {}

    // int nodesLayouted = 1;
    // clock_t t0 = clock();
    // while (v.next()) { nodesLayouted++; }
    // double t = (static_cast<double>(clock()-t0) / CLOCKS_PER_SEC) * 1000.0;
    // double nps = static_cast<double>(nodesLayouted) / 
    //   (static_cast<double>(clock()-t0) / CLOCKS_PER_SEC);
    // std::cout << "Drawing done. " << nodesLayouted << " nodes in "
    //   << t << " ms. " << nps << " nodes/s." << std::endl;

  }

  void
  TreeCanvasImpl::mouseDoubleClickEvent(QMouseEvent* event) {
    if (mutex.tryLock()) {
      if(event->button() == Qt::LeftButton) {
        VisualNode* n = eventNode(event);
        if(n == currentNode) {      
          inspectCurrentNode();
          event->accept();
          mutex.unlock();
          return;
        }
      }
      mutex.unlock();
    }
    event->ignore();
  }
  
  void
  TreeCanvasImpl::contextMenuEvent(QContextMenuEvent* event) {
    if (mutex.tryLock()) {
      VisualNode* n = eventNode(event);
      if (n != NULL) {
        setCurrentNode(n);
        emit contextMenu(event);
        event->accept();
        mutex.unlock();
        return;
      }
      mutex.unlock();
    }
    event->ignore();
  }

  void
  TreeCanvasImpl::finish(void) {
    stopSearchFlag = true;
    searcher.wait();    
  }
  
  void
  TreeCanvasImpl::setCurrentNode(VisualNode* n) {
    QMutexLocker locker(&mutex);
    if (n != NULL) {
      currentNode->setMarked(false);
      currentNode = n;
      currentNode->setMarked(true);
      emit statusChanged(currentNode,stats,true);
      QWidget::update();
    }
  }
  
  void
  TreeCanvasImpl::mousePressEvent(QMouseEvent* event) {
    if (mutex.tryLock()) {
      if (event->button() == Qt::LeftButton) {
        VisualNode* n = eventNode(event);
        setCurrentNode(n);
        if (n != NULL) {
          event->accept();
          mutex.unlock();
          return;
        }
      }
      mutex.unlock();
    }
    event->ignore();
  }
  
  void
  TreeCanvasImpl::setAutoHideFailed(bool b) {
    autoHideFailed = b;
  }
  
  void
  TreeCanvasImpl::setAutoZoom(bool b) {
    autoZoom = b;
    if (autoZoom) {
      zoomToFit();
    }
    emit autoZoomChanged(b);
    scaleBar->setEnabled(!b);
  }

  bool
  TreeCanvasImpl::getAutoHideFailed(void) {
    return autoHideFailed;
  }
  
  bool
  TreeCanvasImpl::getAutoZoom(void) {
    return autoZoom;
  }

  void
  TreeCanvasImpl::setRefresh(int i) {
    refresh = i;
  }

  bool
  TreeCanvasImpl::getSmoothScrollAndZoom(void) {
    return smoothScrollAndZoom;
  }

  void
  TreeCanvasImpl::setSmoothScrollAndZoom(bool b) {
    smoothScrollAndZoom = b;
  }
  
  void
  TreeCanvasImpl::getRootVars(Gecode::Reflection::VarMap& vm) {
    QMutexLocker locker(&mutex);
    if(root != NULL) {
      Space* space = root->getSpace(curBest);
      space->getVars(vm, false);
      delete space;
    }
  }

  void
  TreeCanvasImpl::addVisualisation(QStringList vars, QString visType, QString windowName) {
    Config conf;
    
    pt2createView cv = conf.visualisationMap.value(visType);
    
    if(cv != NULL) {
      Reflection::VarMap vm;
      Space* rootSpace = root->getSpace(curBest);
      rootSpace->getVars(vm, false);
      delete rootSpace;

      QWidget* varView = cv(vm, nextPit, vars, this);

      varView->setWindowTitle(windowName);

      varView->setWindowFlags(Qt::Tool);

      varView->show();

      connect(this, SIGNAL(inspect(Gecode::Reflection::VarMap&, int)),
              varView, SLOT(display(Gecode::Reflection::VarMap&, int)));
      connect(this, SIGNAL(pointInTimeChanged(int)),
              varView, SLOT(displayOld(int)));
      connect(varView, SIGNAL(pointInTimeChanged(int)),
              this, SLOT(markCurrentNode(int)));
    }
  }
  
  void
  TreeCanvasImpl::addVisualisation(void) {

    Config conf;
    
    Reflection::VarMap rootVm;
    getRootVars(rootVm);

    AddVisualisationDialog* addVisDialog = new AddVisualisationDialog(conf, rootVm, this);

    if(addVisDialog->exec()) {

      QStringList itemList = addVisDialog->vars();
      QString visualisation = addVisDialog->vis();
      QString name = addVisDialog->name();

      addVisualisation(itemList, visualisation, name);
    }
  }

  TreeCanvas::TreeCanvas(Space* root, bool bab,
                         QWidget* parent) : QWidget(parent) {
    QGridLayout* layout = new QGridLayout(this);    

    QScrollArea* scrollArea = new QScrollArea(this);
    
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scrollArea->setAlignment(Qt::AlignHCenter);
    scrollArea->setAutoFillBackground(true);
    QPalette myPalette(scrollArea->palette());
    myPalette.setColor(QPalette::Window, Qt::white);
    scrollArea->setPalette(myPalette);
    canvas = new TreeCanvasImpl(root, bab, this);
    canvas->setPalette(myPalette);
    canvas->setObjectName("canvas");

    scrollArea->setWidget(canvas);

    QPixmap myPic;
    myPic.loadFromData(zoomToFitIcon, sizeof(zoomToFitIcon));

    QToolButton* autoZoomButton = new QToolButton();
    autoZoomButton->setCheckable(true);
    autoZoomButton->setIcon(myPic);

    QSlider* scaleBar = new QSlider(Qt::Vertical, this);
    canvas->scaleBar = scaleBar;
    scaleBar->setObjectName("scaleBar");
    scaleBar->setMinimum(minScale);
    scaleBar->setMaximum(maxScale);
    scaleBar->setValue(defScale);
    
    inspectCN = new QAction("Inspect", this);
    inspectCN->setShortcut(QKeySequence("Return"));
    connect(inspectCN, SIGNAL(triggered()), canvas, 
                       SLOT(inspectCurrentNode()));

    stopCN = new QAction("Stop search", this);
    stopCN->setShortcut(QKeySequence("Esc"));
    connect(stopCN, SIGNAL(triggered()), canvas, 
                    SLOT(stopSearch()));

    reset = new QAction("Reset", this);
    reset->setShortcut(QKeySequence("Ctrl+R"));
    connect(reset, SIGNAL(triggered()), canvas, 
            SLOT(reset()));

    navUp = new QAction("Up", this);
    navUp->setShortcut(QKeySequence("Up"));
    connect(navUp, SIGNAL(triggered()), canvas, 
                   SLOT(navUp()));

    navDown = new QAction("Down", this);
    navDown->setShortcut(QKeySequence("Down"));
    connect(navDown, SIGNAL(triggered()), canvas, 
                     SLOT(navDown()));

    navLeft = new QAction("Left", this);
    navLeft->setShortcut(QKeySequence("Left"));
    connect(navLeft, SIGNAL(triggered()), canvas, 
                     SLOT(navLeft()));

    navRight = new QAction("Right", this);
    navRight->setShortcut(QKeySequence("Right"));
    connect(navRight, SIGNAL(triggered()), canvas, 
                      SLOT(navRight()));

    navRoot = new QAction("Root", this);
    navRoot->setShortcut(QKeySequence("R"));
    connect(navRoot, SIGNAL(triggered()), canvas, 
                      SLOT(navRoot()));

    navNextSol = new QAction("To next solution", this);
    navNextSol->setShortcut(QKeySequence("Shift+Right"));
    connect(navNextSol, SIGNAL(triggered()), canvas, 
                      SLOT(navNextSol()));

    navPrevSol = new QAction("To previous solution", this);
    navPrevSol->setShortcut(QKeySequence("Shift+Left"));
    connect(navPrevSol, SIGNAL(triggered()), canvas, 
                      SLOT(navPrevSol()));

    searchNext = new QAction("Next solution", this);
    searchNext->setShortcut(QKeySequence("N"));
    connect(searchNext, SIGNAL(triggered()), canvas, SLOT(searchOne()));

    searchAll = new QAction("All solutions", this);
    searchAll->setShortcut(QKeySequence("A"));
    connect(searchAll, SIGNAL(triggered()), canvas, SLOT(searchAll()));

    toggleHidden = new QAction("Hide/unhide", this);
    toggleHidden->setShortcut(QKeySequence("H"));
    connect(toggleHidden, SIGNAL(triggered()), canvas, SLOT(toggleHidden()));

    hideFailed = new QAction("Hide failed subtrees", this);
    hideFailed->setShortcut(QKeySequence("F"));
    connect(hideFailed, SIGNAL(triggered()), canvas, SLOT(hideFailed()));

    unhideAll = new QAction("Unhide all", this);
    unhideAll->setShortcut(QKeySequence("U"));
    connect(unhideAll, SIGNAL(triggered()), canvas, SLOT(unhideAll()));

    zoomToFit = new QAction("Zoom to fit", this);
    zoomToFit->setShortcut(QKeySequence("Z"));
    connect(zoomToFit, SIGNAL(triggered()), canvas, SLOT(zoomToFit()));

    centerCN = new QAction("Center current node", this);
    centerCN->setShortcut(QKeySequence("C"));
    connect(centerCN, SIGNAL(triggered()), canvas, SLOT(centerCurrentNode()));

    exportPDF = new QAction("Export subtree PDF...", this);
    exportPDF->setShortcut(QKeySequence("P"));
    connect(exportPDF, SIGNAL(triggered()), canvas, 
            SLOT(exportPDF()));

    exportWholeTreePDF = new QAction("Export PDF...", this);
    exportWholeTreePDF->setShortcut(QKeySequence("Ctrl+Shift+P"));
    connect(exportWholeTreePDF, SIGNAL(triggered()), canvas, 
            SLOT(exportWholeTreePDF()));

    print = new QAction("Print...", this);
    print->setShortcut(QKeySequence("Ctrl+P"));
    connect(print, SIGNAL(triggered()), canvas, 
            SLOT(print()));

    setPath = new QAction("Set path", this);
    setPath->setShortcut(QKeySequence("Shift+P"));
    connect(setPath, SIGNAL(triggered()), canvas, SLOT(setPath()));

    inspectPath = new QAction("Inspect path", this);
    inspectPath->setShortcut(QKeySequence("Shift+I"));
    connect(inspectPath, SIGNAL(triggered()), canvas, SLOT(inspectPath()));

    addVisualisation = new QAction("Add visualisation", this);
    addVisualisation->setShortcut(QKeySequence("Shift+V"));
    connect(addVisualisation, SIGNAL(triggered()), canvas, SLOT(addVisualisation()));

    addAction(inspectCN);
    addAction(stopCN);
    addAction(reset);
    addAction(navUp);
    addAction(navDown);
    addAction(navLeft);
    addAction(navRight);
    addAction(navRoot);
    addAction(navNextSol);
    addAction(navPrevSol);

    addAction(searchNext);
    addAction(searchAll);
    addAction(toggleHidden);
    addAction(hideFailed);
    addAction(unhideAll);
    addAction(zoomToFit);
    addAction(centerCN);
    addAction(exportPDF);
    addAction(exportWholeTreePDF);
    addAction(print);

    addAction(addVisualisation);
    
    addAction(setPath);
    addAction(inspectPath);

    contextMenu = new QMenu(this);
    contextMenu->addAction(inspectCN);
    contextMenu->addAction(centerCN);

    contextMenu->addSeparator();

    contextMenu->addAction(searchNext);
    contextMenu->addAction(searchAll);      

    contextMenu->addSeparator();

    contextMenu->addAction(toggleHidden);
    contextMenu->addAction(hideFailed);
    contextMenu->addAction(unhideAll);

    contextMenu->addSeparator();

    contextMenu->addAction(setPath);
    contextMenu->addAction(inspectPath);

    contextMenu->addSeparator();

    connect(scaleBar, SIGNAL(valueChanged(int)), canvas, SLOT(scaleTree(int)));

    connect(canvas, SIGNAL(scaleChanged(int)), scaleBar, SLOT(setValue(int)));

    connect(autoZoomButton, SIGNAL(toggled(bool)), canvas,
            SLOT(setAutoZoom(bool)));

    connect(canvas, SIGNAL(autoZoomChanged(bool)),
            autoZoomButton, SLOT(setChecked(bool)));

    connect(&canvas->searcher, SIGNAL(scaleChanged(int)),
            scaleBar, SLOT(setValue(int)));
    
    layout->addWidget(scrollArea, 0,0,-1,1);
    layout->addWidget(scaleBar, 1,1, Qt::AlignHCenter);
    layout->addWidget(autoZoomButton, 0,1, Qt::AlignHCenter);
    
    setLayout(layout);

    canvas->show();

    resize(500, 400);

    // enables on_<sender>_<signal>() mechanism
    QMetaObject::connectSlotsByName(this);
  }

  void
  TreeCanvas::resizeEvent(QResizeEvent*) {
    canvas->resizeToOuter();
  }

  void
  TreeCanvas::setInspector(Inspector* i0) { canvas->setInspector(i0); }
  
  TreeCanvas::~TreeCanvas(void) { delete canvas; }
  
  void
  TreeCanvas::on_canvas_contextMenu(QContextMenuEvent* event) {
    contextMenu->popup(event->globalPos());    
  }

  void
  TreeCanvas::on_canvas_statusChanged(VisualNode* n, const Statistics& stats,
                                      bool finished) {
    if (!finished) {
      inspectCN->setEnabled(false);
      stopCN->setEnabled(true);
      reset->setEnabled(false);
      navUp->setEnabled(false);
      navDown->setEnabled(false);
      navLeft->setEnabled(false);
      navRight->setEnabled(false);
      navRoot->setEnabled(false);
      navNextSol->setEnabled(false);
      navPrevSol->setEnabled(false);

      searchNext->setEnabled(false);
      searchAll->setEnabled(false);
      toggleHidden->setEnabled(false);
      hideFailed->setEnabled(false);
      unhideAll->setEnabled(false);
      zoomToFit->setEnabled(false);
      centerCN->setEnabled(false);
      exportPDF->setEnabled(false);
      exportWholeTreePDF->setEnabled(false);
      print->setEnabled(false);

      setPath->setEnabled(false);
      inspectPath->setEnabled(false);
      addVisualisation->setEnabled(false);
    } else {
      inspectCN->setEnabled(true);
      stopCN->setEnabled(false);
      reset->setEnabled(true);

      if (n->isOpen() || n->hasOpenChildren()) {
        searchNext->setEnabled(true);
        searchAll->setEnabled(true);
      } else {
        searchNext->setEnabled(false);
        searchAll->setEnabled(false);      
      }
      if (n->getNumberOfChildren() > 0) {
        navDown->setEnabled(true);
        toggleHidden->setEnabled(true);
        hideFailed->setEnabled(true);
        unhideAll->setEnabled(true);            
      } else {
        navDown->setEnabled(false);
        toggleHidden->setEnabled(false);
        hideFailed->setEnabled(false);
        unhideAll->setEnabled(false);      
      }

      VisualNode* p = n->getParent();
      if (p == NULL) {
        navRoot->setEnabled(false);
        navUp->setEnabled(false);
        navRight->setEnabled(false);
        navLeft->setEnabled(false);
      } else {
        navRoot->setEnabled(true);
        navUp->setEnabled(true);
        unsigned int alt = n->getAlternative();
        navRight->setEnabled(alt + 1 < p->getNumberOfChildren());
        navLeft->setEnabled(alt > 0);
      }

      VisualNode* root = n;
      while (!root->isRoot())
        root = root->getParent();
      NextSolCursor nsc(n, false);
      PreorderNodeVisitor<NextSolCursor> nsv(nsc);
      while (nsv.next()) {}
      navNextSol->setEnabled(nsv.getCursor().node() != root);

      NextSolCursor psc(n, true);
      PreorderNodeVisitor<NextSolCursor> psv(psc);
      while (psv.next()) {}
      navPrevSol->setEnabled(psv.getCursor().node() != root);

      zoomToFit->setEnabled(true);
      centerCN->setEnabled(true);
      exportPDF->setEnabled(true);
      exportWholeTreePDF->setEnabled(true);
      print->setEnabled(true);

      setPath->setEnabled(true);
      inspectPath->setEnabled(true);
      addVisualisation->setEnabled(true);
    }
    emit statusChanged(stats,finished);
  }
  
  void
  TreeCanvas::finish(void) {
    canvas->finish();
  }

  void
  TreeCanvas::closeEvent(QCloseEvent* event) {
    canvas->finish();
    event->accept();
  }

  void
  TreeCanvas::setAutoHideFailed(bool b) { canvas->setAutoHideFailed(b); }
  void
  TreeCanvas::setAutoZoom(bool b) { canvas->setAutoZoom(b); }
  bool
  TreeCanvas::getAutoHideFailed(void) { return canvas->getAutoHideFailed(); }
  bool
  TreeCanvas::getAutoZoom(void) { return canvas->getAutoZoom(); }
  void
  TreeCanvas::setRefresh(int i) { canvas->setRefresh(i); }
  bool
  TreeCanvas::getSmoothScrollAndZoom(void) {
    return canvas->getSmoothScrollAndZoom();
  }
  void
  TreeCanvas::setSmoothScrollAndZoom(bool b) {
    canvas->setSmoothScrollAndZoom(b);
  }

}}

// STATISTICS: gist-any