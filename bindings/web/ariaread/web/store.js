/**
 * store.js — B2: 极简 Store + Event Bus
 *
 * 背景：14 个顶层 let 变量（sourcesData / allBooks / currentBook ...）被 5 个模块随意读写，
 * 极易出状态不同步 bug。本文件提供最小侵入式的订阅/发布 + 关键数据的统一读写入口，
 * 各模块都通过 bus 发事件、通过 store.get*/set* 操作状态，而不是直接改顶层 let。
 *
 * 使用：
 *   Bus.emit('shelf:updated', {bookUrl});
 *   Bus.on('shelf:updated', handler);
 *   ShelfStore.markDirty();
 *   ShelfStore.updateCachedCount(bookUrl, cached, total);
 */

const Bus = (() => {
    const listeners = new Map();  // event → Set<handler>
    return {
        on(event, handler) {
            if (!listeners.has(event)) listeners.set(event, new Set());
            listeners.get(event).add(handler);
            return () => listeners.get(event)?.delete(handler);
        },
        off(event, handler) {
            listeners.get(event)?.delete(handler);
        },
        emit(event, payload) {
            const hs = listeners.get(event);
            if (!hs) return;
            for (const h of hs) {
                try { h(payload); } catch (e) { console.error(`[Bus] ${event} handler error:`, e); }
            }
        },
    };
})();

// 书架统一 Store：单点更新 hashCode → DOM 卡片 → 缓存状态
// 不替换 bookshelf.js 中原有 let 变量（太侵入），仅在需要时由外部调用 store 方法广播变更。
const ShelfStore = {
    /** 触发书架数据变更事件（renderBookshelf 监听它自动重渲染） */
    markDirty() {
        if (typeof markShelfDirty === 'function') markShelfDirty();
        Bus.emit('shelf:dirty', {});
    },

    /** 通知某本书的缓存数量变了（由 reader 下载进度或 download SSE 调用） */
    updateCachedCount(bookUrl, cached, total) {
        Bus.emit('shelf:book-cache-changed', { bookUrl, cached, total });
    },

    /** 通知某本书被移除 */
    emitBookRemoved(bookUrl) {
        Bus.emit('shelf:book-removed', { bookUrl });
    },

    /** 通知某本书已加入书架 */
    emitBookAdded(bookUrl) {
        Bus.emit('shelf:book-added', { bookUrl });
    },

    /** 通知某本书换源完成 */
    emitSourceChanged(oldBookUrl, newBookUrl) {
        Bus.emit('shelf:source-changed', { oldBookUrl, newBookUrl });
    },
};

// 暴露到全局，供各模块 script 直接使用
window.Bus = Bus;
window.ShelfStore = ShelfStore;
