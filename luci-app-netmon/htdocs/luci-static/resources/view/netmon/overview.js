'use strict';
'require view';
'require poll';
'require rpc';
'require fs';
'require uci';
'require request';
'require netmon.common as netmon';

var callGetTraffic = rpc.declare({
	object: 'netmon',
	method: 'traffic'
});

var callGetBaseline = rpc.declare({
	object: 'netmon',
	method: 'baseline',
	params: [ 'start' ],
	expect: { found: 0, baseline: {} }
});

var callHostHints = rpc.declare({
	object: 'luci-rpc',
	method: 'getHostHints'
});

var callGetConfig = rpc.declare({
	object: 'uci',
	method: 'get',
	params: [ 'config', 'section', 'option' ]
});

var currentSortCol = null;
var currentSortDir = null;
var currentFilter = '';

return view.extend({
	render: function() {
		return Promise.all([
			uci.load('netmon'),
			request.get(L.resource('view/netmon/ouidata.json')).then(function(res) {
				if (res.ok)
					return res.json();
				return {};
			}).catch(function() { return {}; })
		]).then(function(results) {
			var ouidata = results[1] || {};
			var ouiDb = ouidata.oui || {};
			var hostRules = ouidata.hostname || [];
			var vendorDb = netmon.prepareVendorDb(ouidata);

			var iconUp = E('span', { 'class': 'netmon-sort-icon' }, '↕');
			var iconDown = E('span', { 'class': 'netmon-sort-icon' }, '↕');
			var iconTotalUp = E('span', { 'class': 'netmon-sort-icon' }, '↕');
			var iconTotalDown = E('span', { 'class': 'netmon-sort-icon' }, '↕');

			var thHostname = E('th', { 'class': 'th netmon-th-sort netmon-col-host' }, _('Hostname'));
			var thIp = E('th', { 'class': 'th netmon-col-ip' }, _('IP Address'));
			var thUpload = E('th', { 'class': 'th netmon-th-sort netmon-col-number' }, [ _('Upload'), iconUp ]);
			var thDownload = E('th', { 'class': 'th netmon-th-sort netmon-col-number' }, [ _('Download'), iconDown ]);
			var thTotalUp = E('th', { 'class': 'th netmon-th-sort netmon-col-number' }, [ _('Period up'), iconTotalUp ]);
			var thTotalDown = E('th', { 'class': 'th netmon-th-sort netmon-col-number' }, [ _('Period down'), iconTotalDown ]);

			var summary = E('div', { 'class': 'netmon-metric-grid' }, [
				netmon.makeMetric('netmon-online-count', _('Online'), '#0d9488', '●'),
				netmon.makeMetric('netmon-up-speed', _('Upload speed'), '#2563eb', '↑'),
				netmon.makeMetric('netmon-down-speed', _('Download speed'), '#059669', '↓')
			]);
			var latestDevices = [];
			var applyFilter = function(ev) {
				currentFilter = ev.target.value || '';
				renderRows(latestDevices);
			};
			var filterInput = E('input', {
				'class': 'netmon-filter-input',
				'type': 'search',
				'placeholder': _('Filter hosts...'),
				'value': currentFilter,
				'input': applyFilter,
				'keyup': applyFilter
			});
			var toolbar = E('div', { 'class': 'netmon-toolbar' }, [
				E('label', { 'class': 'netmon-filter' }, [
					E('span', { 'class': 'netmon-filter-icon' }, '⌕'),
					filterInput
				])
			]);

			var table = E('table', { 'class': 'table cbi-section-table netmon-table netmon-table-overview', 'id': 'traffic_table' }, [
				E('tr', { 'class': 'tr table-titles' }, [ thHostname, thIp, thUpload, thDownload, thTotalUp, thTotalDown ]),
				E('tr', { 'class': 'tr placeholder netmon-placeholder' }, [
					E('td', { 'class': 'td', 'colspan': 6 }, E('em', _('Collecting data...')))
				])
			]);

			function handleSort(col) {
				var state = netmon.nextSortState(currentSortCol, currentSortDir, col);
				currentSortCol = state.col;
				currentSortDir = state.dir;
				updateIcons();
				renderRows(latestDevices);
			}

			function updateIcons() {
				[ iconUp, iconDown, iconTotalUp, iconTotalDown ].forEach(function(i) {
					i.textContent = '↕';
					i.classList.remove('netmon-sort-active');
				});

				var target = currentSortCol === 'upload' ? iconUp :
					currentSortCol === 'download' ? iconDown :
					currentSortCol === 'total_up' ? iconTotalUp :
					currentSortCol === 'total_down' ? iconTotalDown : null;

				if (target) {
					target.textContent = currentSortDir === 'asc' ? '↑' : '↓';
					target.classList.add('netmon-sort-active');
				}
			}

			function renderRows(devices) {
				var visibleDevices = netmon.sortDevices(netmon.filterDevices(devices, currentFilter), currentSortCol, currentSortDir);
				var totalUpSpeed = 0;
				var totalDownSpeed = 0;

				visibleDevices.forEach(function(item) {
					totalUpSpeed += Number(item.up_speed) || 0;
					totalDownSpeed += Number(item.down_speed) || 0;
				});

				netmon.setText('netmon-online-count', currentFilter ? String(visibleDevices.length) + ' / ' + String(devices.length) : String(devices.length));
				netmon.setText('netmon-up-speed', netmon.formatSpeed(totalUpSpeed));
				netmon.setText('netmon-down-speed', netmon.formatSpeed(totalDownSpeed));

				var rows = visibleDevices.map(function(item) {
					var upSpeed = [
						item.up_speed > 0 ? E('span', { 'class': 'netmon-arrow-up' }, '↑ ') : '',
						netmon.formatSpeed(item.up_speed)
					];
					var downSpeed = [
						item.down_speed > 0 ? E('span', { 'class': 'netmon-arrow-down' }, '↓ ') : '',
						netmon.formatSpeed(item.down_speed)
					];

					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, netmon.makeHostCell(item)),
						E('td', { 'class': 'td netmon-cell-num' }, item.ip),
						E('td', { 'class': 'td netmon-cell-num netmon-cell-up' }, upSpeed),
						E('td', { 'class': 'td netmon-cell-num netmon-cell-down' }, downSpeed),
						E('td', { 'class': 'td netmon-cell-num netmon-cell-total' }, netmon.formatTraffic(item.total_up)),
						E('td', { 'class': 'td netmon-cell-num netmon-cell-total' }, netmon.formatTraffic(item.total_down))
					]);
				});

				netmon.replaceRows(table, rows, 6, currentFilter ? _('No matching hosts') : _('No online hosts'));
			}

			function refreshTable() {
				return callGetConfig('netmon', 'main', 'enabled').then(function(res) {
					if (res && res.value === '0') {
						latestDevices = [];
						netmon.setText('netmon-online-count', '0');
						netmon.setText('netmon-up-speed', '0 B/s');
						netmon.setText('netmon-down-speed', '0 B/s');
						netmon.showPlaceholder(table, 6, _('Service disabled'));
						return;
					}

					return Promise.all([
						callGetTraffic().catch(function() { return {}; }),
						callHostHints().catch(function() { return {}; }),
						fs.read('/tmp/dhcp.leases').catch(function() { return ''; }),
						callGetConfig('netmon', 'main', 'cycle').catch(function() { return { value: 'daily' }; })
					]).then(function(results) {
						var trafficData = results[0] || {};
						var hostHints = results[1] || {};
						var leasesContent = results[2] || '';
						var cycleConfig = results[3] || { value: 'daily' };

						return callGetBaseline(netmon.getPeriodStart(cycleConfig.value)).then(function(baseRes) {
							latestDevices = netmon.buildDevices({
								trafficData: trafficData,
								hostHints: hostHints,
								leasesContent: leasesContent,
								baseRes: baseRes,
								vendorDb: vendorDb,
								ouiDb: ouiDb,
								hostRules: hostRules
							}).filter(function(item) {
								return !!item.isOnline;
							});

							renderRows(latestDevices);
						});
					});
				}).catch(function() {
					latestDevices = [];
					netmon.showPlaceholder(table, 6, _('Unable to load data'));
				});
			}

			thHostname.onclick = function() {
				currentSortCol = null;
				currentSortDir = null;
				updateIcons();
				renderRows(latestDevices);
			};
			thUpload.onclick = function() { handleSort('upload'); };
			thDownload.onclick = function() { handleSort('download'); };
			thTotalUp.onclick = function() { handleSort('total_up'); };
			thTotalDown.onclick = function() { handleSort('total_down'); };
			updateIcons();

			poll.add(refreshTable, 1);

			return E('div', { 'class': 'cbi-map netmon-app' }, [
				E('link', { 'rel': 'stylesheet', 'href': L.resource('view/netmon/netmon.css') }),
				E('div', { 'class': 'netmon-page-head' }, [
					E('div', {}, [
						E('h2', { 'class': 'netmon-title' }, _('Traffic overview')),
						E('div', { 'class': 'cbi-map-descr netmon-lede netmon-lede-empty' })
					])
				]),
				summary,
				E('div', { 'class': 'cbi-section netmon-section-card' }, [ toolbar, table ])
			]);
		});
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
