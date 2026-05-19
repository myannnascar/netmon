'use strict';
'require view';
'require form';
'require rpc';
'require ui';

return view.extend({
	render: function() {
		var callNetmonClear = rpc.declare({
			object: 'netmon',
			method: 'clear'
		});

		var m, s, o;

		m = new form.Map('netmon', _('Netmon'),
			_('Service and stats period.'));

		s = m.section(form.NamedSection, 'main', 'netmon', _('General Settings'));

		o = s.option(form.Flag, 'enabled', _('Enable'),
			_('Runs traffic accounting.'));
		o.rmempty = false;

		o = s.option(form.ListValue, 'cycle', _('Stats period'),
			_('Rolling window for period totals.'));
		o.value('daily', _('Daily'));
		o.value('weekly', _('Weekly'));
		o.value('monthly', _('Monthly'));
		o.value('yearly', _('Yearly'));
		o.default = 'daily';

		o = s.option(form.DynamicList, 'extra_iface', _('Extra accounting interfaces'),
			_('Also count traffic on proxy or tunnel interfaces. Use this for daed dae0 and similar TUN devices.'));
		o.default = ['dae0', 'nikki', 'mihomo', 'tun0'];
		o.placeholder = 'dae0';

		s = m.section(form.NamedSection, 'main', 'netmon', _('Data Management'));

		o = s.option(form.Button, '_clear', _('Clear stats'),
			_('Erase counters and history.'));
		o.inputstyle = 'negative';
		o.inputtitle = _('Erase');
		o.onclick = function() {
			if (!confirm(_('Erase all statistics? This cannot be undone.')))
				return;
			
			return callNetmonClear().then(function() {
				ui.addNotification(null, E('p', _('Statistics cleared.')), 'info');
			}).catch(function(e) {
				ui.addNotification(null, E('p', _('Clear failed: %s').format(e.message)), 'error');
			});
		};

		return Promise.resolve(m.render()).then(function(root) {
			var wrap = E('div', { 'class': 'netmon-app netmon-settings' });
			wrap.appendChild(E('link', { 'rel': 'stylesheet', 'href': L.resource('view/netmon/netmon.css') }));
			wrap.appendChild(E('div', { 'class': 'netmon-page-head' }, [
				E('div', {}, [
					E('h2', { 'class': 'netmon-title' }, _('Netmon settings')),
					E('div', { 'class': 'cbi-map-descr netmon-lede netmon-lede-empty' })
				])
			]));
			if (root != null) {
				if (Array.isArray(root))
					root.forEach(function(n) { if (n) wrap.appendChild(n); });
				else
					wrap.appendChild(root);
			}

			var sections = wrap.querySelectorAll('.cbi-section');
			if (sections.length)
				sections[sections.length - 1].classList.add('netmon-danger-section');

			return wrap;
		});
	}
});
