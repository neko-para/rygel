// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

var drdw = {};
(function() {
    'use strict';

    var route_modules = {};

    var route_timer_id = null;

    var route_url = null;
    var route_url_parts = null;
    var route_values = {
        'view': 'table',
        'date': null,
        'ghm_root': null,
        'diff': null,
        'apply_coefficient': false,

        'list': 'classifier_tree',
        'page': 1,
        'spec': null,

        'cm_view': 'summary',
        'period': [null, null],
        'prev_period': [null, null],
        'mode': 'none',
        'units': [],
        'algorithm': null,

        'apply': false
    };
    var scroll_cache = {};
    var module = null;

    function markBusy(selector, busy)
    {
        queryAll(selector).toggleClass('busy', busy);
    }
    this.markBusy = markBusy;

    function toggleMenu(selector, enable)
    {
        var el = query(selector);
        if (enable === undefined)
            enable = !el.classList.contains('active');
        if (enable) {
            var els = queryAll('nav');
            for (var i = 0; i < els.length; i++)
                els[i].toggleClass('active', els[i] == el);
        } else {
            el.removeClass('active');
        }
    }
    this.toggleMenu = toggleMenu;

    function baseUrl(module_name)
    {
        return BaseUrl + module_name;
    }
    this.baseUrl = baseUrl;

    function buildRoute(args)
    {
        if (args === undefined)
            args = {};

        return Object.assign({}, route_values, args);
    }
    this.buildRoute = buildRoute;

    function registerUrl(prefix, object, func)
    {
        route_modules[prefix] = {
            object: object,
            func: func
        };
    }
    this.registerUrl = registerUrl;

    function route(new_url, delay)
    {
        if (new_url === undefined)
            new_url = null;

        if (route_timer_id) {
            clearTimeout(route_timer_id);
            route_timer_id = null;
        }
        if (delay) {
            route_timer_id = setTimeout(function() {
                route(new_url);
            }, delay);
            return;
        }

        // Parse new URL
        let url_parts = new_url ? parseUrl(new_url) : route_url_parts;
        let app_url = url_parts.path.substr(BaseUrl.length);

        // Update scroll cache and history
        if (!route_url_parts || url_parts.href !== route_url_parts.href) {
            if (route_url_parts)
                scroll_cache[route_url_parts.path] = [window.pageXOffset, window.pageYOffset];
            window.history.pushState(null, null, url_parts.href);
        }

        // Update user stuff
        user.runSession();

        // Find relevant module and run
        {
            let new_module_name = app_url.split('/')[0];
            let new_module = route_modules[new_module_name];

            if (new_module !== module)
                queryAll('main > div').addClass('hide');
            queryAll('#opt_menu > *').addClass('hide');

            module = new_module;
            if (module)
                module.func(route_values, app_url, url_parts.params, url_parts.hash);
        }

        // Update URL to reflect real state (module may have set default values, etc.)
        {
            let real_url = null;
            if (module)
                real_url = module.object.routeToUrl({});
            if (real_url) {
                if (url_parts.hash)
                    real_url += '#' + url_parts.hash;

                window.history.replaceState(null, null, real_url);
                route_url_parts = parseUrl(real_url);
                route_url = real_url.substr(BaseUrl.length);
            } else {
                route_url_parts = url_parts;
                route_url = app_url;
            }
        }

        // Update side menu state and links
        var menu_anchors = queryAll('#side_menu li a');
        for (var i = 0; i < menu_anchors.length; i++) {
            let anchor = menu_anchors[i];

            if (anchor.dataset.url) {
                let url = eval(anchor.dataset.url);
                anchor.classList.toggle('hide', !url);
                if (url)
                    anchor.href = url;
            }

            let active = (route_url_parts.href.startsWith(anchor.href) &&
                          !anchor.hasClass('category'));
            anchor.toggleClass('active', active);
        }
        toggleMenu('#side_menu', false);

        // Hide page menu if empty
        let opt_hide = true;
        {
            let els = queryAll('#opt_menu > *');
            for (let i = 0; i < els.length; i++) {
                if (!els[i].hasClass('hide')) {
                    opt_hide = false;
                    break;
                }
            }
        }
        queryAll('#opt_deploy, #opt_menu').toggleClass('hide', opt_hide);

        // Update scroll target
        var scroll_target = scroll_cache[route_url_parts.path];
        if (route_url_parts.hash) {
            window.location.hash = route_url_parts.hash;
        } else if (scroll_target) {
            window.scrollTo(scroll_target[0], scroll_target[1]);
        } else {
            window.scrollTo(0, 0);
        }
    }
    this.route = route;

    function go(args, delay) {
        module.object.go(args, delay);
    }
    this.go = go;

    function refreshErrors(errors)
    {
        var log = query('#log');

        log.innerHTML = errors.join('<br/>');
        log.toggleClass('hide', !errors.length);
    }
    this.refreshErrors = refreshErrors;

    function init()
    {
        let new_url;
        if (window.location.pathname !== BaseUrl) {
            new_url = window.location.href;
        } else {
            var first_anchor = query('#side_menu a[data-url]');
            new_url = eval(first_anchor.dataset.url);
            window.history.replaceState(null, null, new_url);
        }
        route(new_url, false);

        window.addEventListener('popstate', function(e) {
            route(window.location.href, false);
        });
        document.body.addEventListener('click', function(e) {
            if (e.target && e.target.tagName == 'A') {
                let href = e.target.getAttribute('href');
                if (href && !href.match(/^(?:[a-z]+:)?\/\//) && href[0] != '#') {
                    route(href);
                    e.preventDefault();
                }
            }
        });

        data.idleHandler = route;
    }

    if (document.readyState === 'complete') {
        init();
    } else {
        document.addEventListener('DOMContentLoaded', init);
    }
}).call(drdw);
