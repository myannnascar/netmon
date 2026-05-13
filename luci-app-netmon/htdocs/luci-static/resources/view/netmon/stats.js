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

			var iconTotalUp = E('span', { 'class': 'netmon-sort-icon' }, '↕');
			var iconTotalDown = E('span', { 'class': 'netmon-sort-icon' }, '↕');

			var thHostname = E('th', { 'class': 'th netmon-th-sort netmon-col-host' }, _('Hostname'));
			var thIp = E('th', { 'class': 'th netmon-col-ip' }, _('IP Address'));
			var thTotalUp = E('th', { 'class': 'th netmon-th-sort netmon-col-number' }, [ _('Period up'), iconTotalUp ]);
			var thTotalDown = E('th', { 'class': 'th netmon-th-sort netmon-col-number' }, [ _('Period down'), iconTotalDown ]);

			var table = E('table', { 'class': 'table cbi-section-table netmon-table netmon-table-stats', 'id': 'traffic_table' }, [
				E('tr', { 'class': 'tr table-titles' }, [ thHostname, thIp, thTotalUp, thTotalDown ]),
				E('tr', { 'class': 'tr placeholder netmon-placeholder' }, [
					E('td', { 'class': 'td', 'colspan': 4 }, E('em', _('Collecting data...')))
				])
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

			function handleSort(col) {
				var state = netmon.nextSortState(currentSortCol, currentSortDir, col);
				currentSortCol = state.col;
				currentSortDir = state.dir;
				updateIcons();
				renderRows(latestDevices);
			}

			function updateIcons() {
				[ iconTotalUp, iconTotalDown ].forEach(function(i) {
					i.textContent = '↕';
					i.classList.remove('netmon-sort-active');
				});

				var target = currentSortCol === 'total_up' ? iconTotalUp :
					currentSortCol === 'total_down' ? iconTotalDown : null;

				if (target) {
					target.textContent = currentSortDir === 'asc' ? '↑' : '↓';
					target.classList.add('netmon-sort-active');
				}
			}

			function renderRows(devices) {
				var visibleDevices = netmon.sortDevices(netmon.filterDevices(devices, currentFilter), currentSortCol, currentSortDir);
				var rows = visibleDevices.map(function(item) {
					return E('tr', { 'class': 'tr' + (item.isOnline ? '' : ' netmon-row-offline') }, [
						E('td', { 'class': 'td' }, netmon.makeHostCell(item, { showStatus: true })),
						E('td', { 'class': 'td netmon-cell-num' }, item.ip),
						E('td', { 'class': 'td netmon-cell-num netmon-cell-up' }, netmon.formatTraffic(item.total_up)),
						E('td', { 'class': 'td netmon-cell-num netmon-cell-down' }, netmon.formatTraffic(item.total_down))
					]);
				});

				netmon.replaceRows(table, rows, 4, currentFilter ? _('No matching hosts') : _('No hosts'));
			}

			function refreshTable() {
				return callGetConfig('netmon', 'main', 'enabled').then(function(res) {
					if (res && res.value === '0') {
						latestDevices = [];
						netmon.showPlaceholder(table, 4, _('Service disabled'));
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
							});

							renderRows(latestDevices);
						});
					});
				}).catch(function() {
					latestDevices = [];
					netmon.showPlaceholder(table, 4, _('Unable to load data'));
				});
			}

			thHostname.onclick = function() {
				currentSortCol = null;
				currentSortDir = null;
				updateIcons();
				renderRows(latestDevices);
			};
			thTotalUp.onclick = function() { handleSort('total_up'); };
			thTotalDown.onclick = function() { handleSort('total_down'); };
			updateIcons();

			poll.add(refreshTable, 60);

			return E('div', { 'class': 'cbi-map netmon-app' }, [
				E('link', { 'rel': 'stylesheet', 'href': L.resource('view/netmon/netmon.css') }),
				E('div', { 'class': 'netmon-page-head' }, [
					E('div', {}, [
						E('h2', { 'class': 'netmon-title' }, _('Traffic statistics')),
						E('div', { 'class': 'cbi-map-descr netmon-lede netmon-lede-empty' })
					])
				]),
				E('div', { 'class': 'cbi-section netmon-section-card' }, [ toolbar, table ])
			]);
		});
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
