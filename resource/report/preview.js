// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

(() => {
    document.addEventListener('click', function(event) {
        if (event.defaultPrevented || event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey)
            return;
        const link = event.target instanceof Element ? event.target.closest('a[href^="#"]') : null;
        if (!link)
            return;
        const target = document.getElementById(link.getAttribute('href').slice(1));
        if (!target)
            return;
        event.preventDefault();
        for (let parent = target; parent; parent = parent.parentElement) {
            if (parent.tagName === 'DETAILS')
                parent.open = true;
        }
        target.scrollIntoView({block: 'start'});
    });

    const reader = document.getElementById('document-preview');
    if (!reader)
        return;
    const data = document.getElementById('document-page-data');
    if (!data)
        return;
    const status = reader.querySelector('[role="status"]');
    if (typeof DecompressionStream !== 'function') {
        status.textContent = 'This paginated preview requires a current browser. Circuit sheets below remain available.';
        return;
    }
    let pages;
    try {
        pages = JSON.parse(data.textContent);
        if (!Array.isArray(pages) || pages.length === 0)
            throw new Error('Invalid page list');
    } catch (error) {
        status.textContent = 'The paginated preview data could not be read. Circuit sheets below remain available.';
        return;
    }
    data.remove();
    const viewport = reader.querySelector('.document-page');
    const input = reader.querySelector('input');
    const previous = reader.querySelector('[data-page-step="-1"]');
    const next = reader.querySelector('[data-page-step="1"]');
    const zoom = reader.querySelector('select');
    let selected = 1;
    let displayed = 0;
    let rendering = false;

    function resize() {
        const svg = viewport.querySelector('svg');
        if (!svg)
            return;
        viewport.classList.toggle('fit', zoom.value === 'fit');
        if (zoom.value !== 'fit')
            viewport.style.setProperty('--page-width', (svg.viewBox.baseVal.width * Number(zoom.value)) + 'px');
    }

    function pageSource(number) {
        const bytes = Uint8Array.from(atob(pages[number - 1]), c => c.charCodeAt(0));
        const stream = new Blob([bytes.subarray(4)]).stream().pipeThrough(new DecompressionStream('deflate'));
        return new Response(stream).text();
    }

    async function showPage(number) {
        if (!Number.isInteger(Number(number))) {
            input.value = selected;
            return;
        }
        selected = Math.max(1, Math.min(pages.length, Number(number)));
        input.value = selected;
        previous.disabled = selected === 1;
        next.disabled = selected === pages.length;
        if (rendering)
            return;
        rendering = true;
        viewport.setAttribute('aria-busy', 'true');
        try {
            while (displayed !== selected) {
                const page = selected;
                status.textContent = 'Loading page ' + page + ' of ' + pages.length;
                try {
                    const source = await pageSource(page);
                    if (page !== selected)
                        continue;
                    const svg = new DOMParser().parseFromString(source, 'image/svg+xml');
                    if (svg.querySelector('parsererror') || svg.documentElement.localName !== 'svg')
                        throw new Error('Invalid page');
                    const image = document.importNode(svg.documentElement, true);
                    image.setAttribute('role', 'img');
                    image.setAttribute('aria-label', 'Document page ' + page + ' of ' + pages.length);
                    viewport.replaceChildren(image);
                    resize();
                    displayed = page;
                } catch (error) {
                    if (page !== selected)
                        continue;
                    viewport.replaceChildren();
                    displayed = 0;
                    status.textContent = 'The requested page could not be displayed. Circuit sheets below remain available.';
                    return;
                }
            }
            status.textContent = 'Page ' + displayed + ' of ' + pages.length;
        } finally {
            rendering = false;
            viewport.setAttribute('aria-busy', 'false');
        }
    }

    input.disabled = false;
    zoom.disabled = false;
    input.addEventListener('change', () => showPage(input.value));
    input.addEventListener('keydown', event => {
        if (event.key === 'Enter')
            showPage(input.value);
    });
    previous.addEventListener('click', () => showPage(selected - 1));
    next.addEventListener('click', () => showPage(selected + 1));
    zoom.addEventListener('change', resize);
    document.addEventListener('click', event => {
        if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey)
            return;
        const link = event.target instanceof Element ? event.target.closest('[data-document-page]') : null;
        if (link)
            showPage(Number(link.dataset.documentPage));
    });
    showPage(1);
})();
