'use strict';
'require baseclass';
'require ui';
'require uci';
'require rpc';

var callUciCommit = rpc.declare({
	object: 'uci',
	method: 'commit',
	params: [ 'config' ]
});

function normalizeIPv6(ip) {
	if (!ip || ip.indexOf(':') === -1)
		return ip ? ip.toLowerCase() : ip;

	var parts = ip.split(':');
	var fullParts = [];
	var doubleColonIdx = -1;

	for (var i = 0; i < parts.length; i++) {
		if (parts[i] === '') {
			if (doubleColonIdx === -1)
				doubleColonIdx = i;
			continue;
		}
		fullParts.push(parts[i]);
	}

	if (doubleColonIdx !== -1) {
		var missing = 8 - fullParts.length;
		var left = parts.slice(0, doubleColonIdx);
		var right = parts.slice(doubleColonIdx + 1).filter(function(s) { return s !== ''; });
		fullParts = left;
		for (var j = 0; j < missing; j++)
			fullParts.push('0');
		fullParts = fullParts.concat(right);
	}

	return fullParts.map(function(p) {
		var s = p || '0';
		while (s.length < 4 && s !== '0')
			s = '0' + s;
		return s.toLowerCase();
	}).join(':').replace(/^(0+)/, '').replace(/:0+/g, ':0');
}

function isDisplayableIP(ip) {
	if (!ip)
		return false;

	if (ip.indexOf('.') !== -1) {
		var firstOctet = parseInt(ip.split('.')[0]);
		if (firstOctet >= 224 || ip === '255.255.255.255')
			return false;
	}

	if (ip.indexOf(':') !== -1 && ip.toLowerCase().indexOf('ff') === 0)
		return false;

	return true;
}

function formatBytes(bytes, suffixes) {
	if (bytes == null || bytes === 0)
		return '0 ' + suffixes[0];

	var k = 1024;
	var i = Math.floor(Math.log(bytes) / Math.log(k));
	return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + suffixes[i];
}

function formatSpeed(bytes) {
	return formatBytes(bytes, [ 'B/s', 'KB/s', 'MB/s', 'GB/s', 'TB/s' ]);
}

function formatTraffic(bytes) {
	return formatBytes(bytes, [ 'B', 'KB', 'MB', 'GB', 'TB', 'PB' ]);
}

function toSortableNumber(v) {
	var n = Number(v);
	return Number.isFinite(n) ? n : 0;
}

function setText(id, text) {
	var el = document.getElementById(id);
	if (el)
		el.textContent = text;
}

function makeMetric(id, label, tone, glyph, extraClass) {
	return E('div', {
		'class': 'netmon-metric' + (extraClass ? ' ' + extraClass : ''),
		'style': '--netmon-metric-tone: ' + tone + ';'
	}, [
		E('div', { 'class': 'netmon-metric-top' }, [
			E('div', { 'class': 'netmon-metric-label' }, label),
			E('span', { 'class': 'netmon-metric-glyph' }, glyph)
		]),
		E('div', { 'id': id, 'class': 'netmon-metric-value' }, '-')
	]);
}

function getPeriodStart(cycle) {
	var nowSec = Math.floor(Date.now() / 1000);
	var duration = 86400;

	if (cycle === 'weekly')
		duration = 604800;
	else if (cycle === 'monthly')
		duration = 2592000;
	else if (cycle === 'yearly')
		duration = 31536000;

	return nowSec - duration;
}

function buildHostMaps(leasesContent, hostHints) {
	var hostMap = {};
	var ipToMac = {};

	(leasesContent || '').split('\n').forEach(function(line) {
		var p = line.trim().split(/\s+/);
		if (p.length >= 4 && p[3] && p[3] !== '*') {
			var hostname = p[3];
			var ip = p[2].toLowerCase();
			var mac = p[1].toLowerCase();
			hostMap[ip] = hostname;
			ipToMac[ip] = mac;
		}
	});

	Object.keys(hostHints || {}).forEach(function(mac) {
		var hint = hostHints[mac] || {};
		var hostname = hint.name;

		if (hint.ipaddrs) {
			hint.ipaddrs.forEach(function(ip) {
				ipToMac[ip.toLowerCase()] = mac;
				if (hostname)
					hostMap[ip.toLowerCase()] = hostname;
			});
		}

		if (hint.ip6addrs) {
			hint.ip6addrs.forEach(function(ip) {
				ipToMac[ip.toLowerCase()] = mac;
				ipToMac[normalizeIPv6(ip)] = mac;
				if (hostname) {
					hostMap[ip.toLowerCase()] = hostname;
					hostMap[normalizeIPv6(ip)] = hostname;
				}
			});
		}
	});

	return {
		hostMap: hostMap,
		ipToMac: ipToMac
	};
}

function getAliasMap() {
	var aliasMap = {};

	uci.sections('netmon', 'device', function(s) {
		if (s.mac && s.alias)
			aliasMap[s.mac.toLowerCase()] = s.alias;
	});

	return aliasMap;
}

function normalizeMacPrefix(prefix) {
	return String(prefix || '').replace(/[^0-9a-f]/gi, '').toUpperCase();
}

function normalizeVendorName(name) {
	var vendor = String(name || '').trim();

	return vendor
		.replace(/\s+/g, ' ')
		.replace(/\b(Co\.?,?\s*Ltd\.?|Corporation|Corp\.?|Incorporated|Inc\.?|Limited|Ltd\.?|Technology|Technologies)\b\.?/gi, '')
		.replace(/\s*,\s*$/g, '')
		.trim();
}

function addVendorPrefix(groups, prefix, vendor) {
	var p = normalizeMacPrefix(prefix);
	var v = normalizeVendorName(vendor);

	if (!p || !v)
		return;

	var len = String(p.length);
	if (!groups[len])
		groups[len] = {};

	groups[len][p] = v;
}

function prepareVendorDb(data) {
	data = data || {};

	var groups = {};
	var legacyOui = data.oui || {};
	var i;

	Object.keys(legacyOui).forEach(function(prefix) {
		addVendorPrefix(groups, prefix, legacyOui[prefix]);
	});

	var maps = data.prefixes || data.mac_prefixes || {};
	Object.keys(maps).forEach(function(len) {
		var map = maps[len] || {};
		Object.keys(map).forEach(function(prefix) {
			addVendorPrefix(groups, prefix, map[prefix]);
		});
	});

	var list = data.mac_prefix || data.prefix_list || [];
	for (i = 0; i < list.length; i++) {
		var item = list[i];
		if (Array.isArray(item))
			addVendorPrefix(groups, item[0], item[item.length - 1]);
		else if (item)
			addVendorPrefix(groups, item.prefix, item.vendor);
	}

	var lengths = Object.keys(groups).map(function(len) {
		return parseInt(len);
	}).filter(function(len) {
		return len > 0;
	}).sort(function(a, b) {
		return b - a;
	});

	return {
		prefixGroups: groups,
		prefixLengths: lengths,
		hostRules: data.hostname || [],
		aliases: data.vendor_aliases || {}
	};
}

function lookupVendorByMac(mac, vendorDb, legacyOui) {
	if (!mac)
		return null;

	var m = normalizeMacPrefix(mac);
	if (m.length < 6)
		return null;

	vendorDb = vendorDb || {};
	var groups = vendorDb.prefixGroups || {};
	var lengths = vendorDb.prefixLengths || [];

	for (var i = 0; i < lengths.length; i++) {
		var len = lengths[i];
		if (m.length >= len && groups[String(len)] && groups[String(len)][m.substring(0, len)])
			return groups[String(len)][m.substring(0, len)];
	}

	return (legacyOui || {})[m.substring(0, 6)] || null;
}

function buildSnapMap(baseRes) {
	var snapMap = {};
	var snapMacMap = {};
	var bestSnap = (baseRes && baseRes.found && baseRes.baseline) ? baseRes.baseline : null;

	if (bestSnap && bestSnap.devices) {
		bestSnap.devices.forEach(function(d) {
			snapMap[d.ip.toLowerCase()] = d;
			if (d.mac)
				snapMacMap[d.mac.toLowerCase()] = d;
		});
	}

	return {
		snapMap: snapMap,
		snapMacMap: snapMacMap,
		timestamp: bestSnap ? bestSnap.timestamp : 0
	};
}

function preferDeviceIp(currentIp, candidateIp) {
	if (!candidateIp)
		return currentIp;

	if (!currentIp)
		return candidateIp;

	if (currentIp.indexOf(':') !== -1 && candidateIp.indexOf('.') !== -1)
		return candidateIp;

	return currentIp;
}

function getVendorInfo(mac, hostname, vendorDb, ouiDb, hostRules) {
	var vendor = null;
	var isRandom = false;

	if (mac) {
		var m = normalizeMacPrefix(mac);
		if (m.length >= 6) {
			var c = m.charAt(1);
			if (c === '2' || c === '6' || c === 'A' || c === 'E') {
				isRandom = true;
			}
			else {
				vendor = lookupVendorByMac(mac, vendorDb, ouiDb);
			}
		}
	}

	if (hostname) {
		hostRules = hostRules || (vendorDb && vendorDb.hostRules) || [];
		for (var i = 0; i < (hostRules || []).length; i++) {
			var rule = hostRules[i];
			if (new RegExp(rule.pattern, 'i').test(hostname)) {
				if (!vendor)
					vendor = normalizeVendorName(rule.vendor);
				break;
			}
		}
	}

	return {
		vendor: vendor,
		isRandom: isRandom
	};
}

function buildDevices(opts) {
	var trafficData = opts.trafficData || {};
	var hostHints = opts.hostHints || {};
	var maps = buildHostMaps(opts.leasesContent || '', hostHints);
	var aliasMap = getAliasMap();
	var baseline = buildSnapMap(opts.baseRes);

	var getHostname = function(ip) {
		var lowerIp = ip.toLowerCase();
		if (maps.hostMap[lowerIp])
			return maps.hostMap[lowerIp];

		var normIp = (ip.indexOf(':') !== -1) ? normalizeIPv6(ip) : lowerIp;
		return maps.hostMap[normIp] || null;
	};

	var getHostnameByMac = function(mac) {
		var hint = mac ? hostHints[mac] || hostHints[mac.toLowerCase()] || hostHints[mac.toUpperCase()] : null;
		return hint && hint.name ? hint.name : null;
	};

	return (trafficData.devices || []).filter(function(dev) {
		return isDisplayableIP(dev.ip);
	}).map(function(dev) {
		var lowerIp = dev.ip.toLowerCase();
		var normIp = (dev.ip.indexOf(':') !== -1) ? normalizeIPv6(dev.ip) : lowerIp;
		var mac = dev.mac || maps.ipToMac[lowerIp] || (normIp !== lowerIp ? maps.ipToMac[normIp] : null);
		var hostname = getHostnameByMac(mac) || getHostname(dev.ip) || _('Unknown');

		if (!mac) {
			Object.keys(hostHints).forEach(function(m) {
				var hint = hostHints[m];
				if (hint.ipaddrs && hint.ipaddrs.indexOf(dev.ip) !== -1)
					mac = m;
				else if (hint.ip6addrs && hint.ip6addrs.indexOf(dev.ip) !== -1)
					mac = m;
			});
		}

		var snap = mac ? baseline.snapMacMap[mac.toLowerCase()] : null;
		if (!snap)
			snap = baseline.snapMap[lowerIp] || (normIp !== lowerIp ? baseline.snapMap[normIp] : null);

		var totalUp = dev.total_up;
		var totalDown = dev.total_down;

		if (snap && baseline.timestamp > 0) {
			totalUp = (dev.total_up >= snap.total_up) ? (dev.total_up - snap.total_up) : dev.total_up;
			totalDown = (dev.total_down >= snap.total_down) ? (dev.total_down - snap.total_down) : dev.total_down;
		}

		var isOnline = (dev.online === undefined || dev.online === null) ? true : !!dev.online;
		if (!mac)
			isOnline = false;

		var vendorInfo = getVendorInfo(mac, hostname, opts.vendorDb || null, opts.ouiDb || {}, opts.hostRules || []);

		return {
			ip: dev.ip,
			mac: mac,
			hostname: hostname,
			alias: mac ? aliasMap[mac.toLowerCase()] : null,
			vendor: vendorInfo.vendor,
			isRandomized: !!vendorInfo.isRandom,
			up_speed: dev.up_speed,
			down_speed: dev.down_speed,
			total_up: totalUp,
			total_down: totalDown,
			isOnline: isOnline
		};
	});
}

function aggregateDevices(devices) {
	var grouped = {};
	var order = [];

	(devices || []).forEach(function(item) {
		var mac = item.mac ? item.mac.toLowerCase() : '';
		var key = mac ? ('mac:' + mac) : ('ip:' + String(item.ip || '').toLowerCase());
		var group = grouped[key];

		if (!group) {
			group = {
				ip: item.ip,
				mac: item.mac || null,
				hostname: item.hostname,
				alias: item.alias,
				vendor: item.vendor,
				isRandomized: item.isRandomized,
				up_speed: 0,
				down_speed: 0,
				total_up: 0,
				total_down: 0,
				isOnline: false,
				stackRows: []
			};
			grouped[key] = group;
			order.push(key);
		}

		group.ip = preferDeviceIp(group.ip, item.ip);
		if (!group.alias && item.alias)
			group.alias = item.alias;
		if ((group.hostname === _('Unknown') || !group.hostname) && item.hostname)
			group.hostname = item.hostname;
		if (!group.vendor && item.vendor)
			group.vendor = item.vendor;
		group.isRandomized = group.isRandomized || item.isRandomized;
		group.isOnline = group.isOnline || item.isOnline;
		group.up_speed += Number(item.up_speed) || 0;
		group.down_speed += Number(item.down_speed) || 0;
		group.total_up += Number(item.total_up) || 0;
		group.total_down += Number(item.total_down) || 0;
		group.stackRows.push({
			ip: item.ip,
			up_speed: item.up_speed,
			down_speed: item.down_speed,
			total_up: item.total_up,
			total_down: item.total_down,
			isIPv6: item.ip && item.ip.indexOf(':') !== -1
		});
	});

	return order.map(function(key) {
		var group = grouped[key];
		group.stackRows.sort(function(a, b) {
			if (!!a.isIPv6 !== !!b.isIPv6)
				return a.isIPv6 ? 1 : -1;
			return String(a.ip || '').localeCompare(String(b.ip || ''), undefined, { numeric: true });
		});
		return group;
	});
}

function sortDevices(devices, sortCol, sortDir) {
	var list = devices.slice();

	list.sort(function(a, b) {
		if (sortCol) {
			var key = sortCol === 'upload' ? 'up_speed' :
				sortCol === 'download' ? 'down_speed' :
				sortCol === 'total_up' ? 'total_up' : 'total_down';
			var valA = toSortableNumber(a[key]);
			var valB = toSortableNumber(b[key]);
			return sortDir === 'desc' ? (valB - valA) : (valA - valB);
		}

		if (a.isOnline !== b.isOnline)
			return a.isOnline ? -1 : 1;

		var unknownStr = _('Unknown');
		var isKnownA = a.alias || (a.hostname && a.hostname !== unknownStr);
		var isKnownB = b.alias || (b.hostname && b.hostname !== unknownStr);
		if (!!isKnownA !== !!isKnownB)
			return isKnownA ? -1 : 1;

		var nameA = (a.alias || a.hostname || '').toLowerCase();
		var nameB = (b.alias || b.hostname || '').toLowerCase();
		if (nameA !== nameB)
			return nameA.localeCompare(nameB);

		return a.ip.localeCompare(b.ip, undefined, { numeric: true });
	});

	return list;
}

function nextSortState(currentCol, currentDir, col) {
	if (currentCol !== col)
		return { col: col, dir: 'desc' };

	if (currentDir === 'desc')
		return { col: col, dir: 'asc' };

	return { col: null, dir: null };
}

function filterDevices(devices, query) {
	var q = (query || '').trim().toLowerCase();

	if (!q)
		return devices.slice();

	return devices.filter(function(item) {
		var text = [
			item.alias,
			item.hostname,
			item.ip,
			item.mac,
			item.vendor,
			item.isOnline ? _('Online') : _('Offline'),
			item.isRandomized ? _('Randomized') : ''
		].filter(Boolean).join(' ').toLowerCase();

		return text.indexOf(q) !== -1;
	});
}

function openAliasPrompt(item) {
	if (!item.mac) {
		var warn = ui.addNotification(null, E('p', _('No MAC for this host')), 'warning');
		setTimeout(function() { if (warn && warn.parentNode) warn.parentNode.removeChild(warn); }, 3000);
		return;
	}

	var input = E('input', {
		'class': 'cbi-input-text',
		'type': 'text',
		'style': 'width: 100%;',
		'value': item.alias || ''
	});

	var saveAlias = function() {
		var newAlias = (input.value || '').trim();
		var section = null;

		uci.sections('netmon', 'device', function(s) {
			if (s.mac && s.mac.toLowerCase() === item.mac.toLowerCase())
				section = s['.name'];
		});

		var changed = false;
		if (section) {
			if (newAlias === '')
				uci.unset('netmon', section, 'alias');
			else
				uci.set('netmon', section, 'alias', newAlias);
			changed = true;
		}
		else if (newAlias !== '') {
			var sid = uci.add('netmon', 'device');
			uci.set('netmon', sid, 'mac', item.mac);
			uci.set('netmon', sid, 'alias', newAlias);
			changed = true;
		}

		if (!changed) {
			ui.hideModal();
			return;
		}

		uci.save().then(function() {
			return callUciCommit('netmon');
		}).then(function() {
			var info = ui.addNotification(null, E('p', String.format(_('Alias saved: %s'), item.ip)), 'info');
			setTimeout(function() { if (info && info.parentNode) info.parentNode.removeChild(info); }, 3000);
		}).catch(function(e) {
			var msg = (e && e.message) ? e.message : e;
			var err = ui.addNotification(null, E('p', String.format(_('Alias save failed (%s): %s'), item.ip, msg || '-')), 'error');
			setTimeout(function() { if (err && err.parentNode) err.parentNode.removeChild(err); }, 5000);
		});

		ui.hideModal();
	};

	ui.showModal(_('Alias'), [
		E('p', {}, String.format(_('%s / %s'), item.ip, item.mac)),
		input,
		E('div', { 'style': 'margin-top: 10px; text-align: right;' }, [
			E('button', { 'class': 'cbi-button cbi-button-positive', 'click': function(ev) { ev.preventDefault(); saveAlias(); } }, _('Save')),
			' ',
			E('button', { 'class': 'cbi-button', 'click': function(ev) { ev.preventDefault(); ui.hideModal(); } }, _('Cancel'))
		])
	]);

	input.focus();
}

function makeHostCell(item, opts) {
	opts = opts || {};
	var nameTextContent = item.hostname;
	var vendorText = item.vendor || '';
	var showHostnameMeta = item.alias && nameTextContent &&
		nameTextContent !== item.alias &&
		nameTextContent !== vendorText &&
		nameTextContent !== _('Randomized') &&
		vendorText.indexOf(nameTextContent) === -1;
	var nameText = E('span', {
		'class': 'netmon-host-name',
		'title': item.alias ? (item.alias + ' (' + item.hostname + ')') : (item.vendor ? (item.hostname + ' (' + item.vendor + ')') : item.hostname)
	}, item.alias ? [
		E('strong', item.alias),
		showHostnameMeta ? E('small', { 'class': 'netmon-host-meta' }, '(' + nameTextContent + ')') : ''
	] : [
		document.createTextNode(nameTextContent)
	]);

	var editIcon = E('span', {
		'class': 'netmon-alias-trigger',
		'tabindex': '0',
		'role': 'button',
		'title': _('Edit alias'),
		'aria-label': _('Edit alias'),
		'click': function(ev) { ev.preventDefault(); openAliasPrompt(item); },
		'keydown': function(ev) {
			if (ev.key === 'Enter' || ev.key === ' ') {
				ev.preventDefault();
				openAliasPrompt(item);
			}
		}
	}, '\u270E');

	var metaItems = [];

	if (opts.showStatus === true)
		metaItems.push(E('span', {
			'class': 'netmon-host-state ' + (item.isOnline ? 'netmon-host-state-online' : 'netmon-host-state-offline'),
			'title': item.isOnline ? _('Online') : _('Offline'),
			'aria-label': item.isOnline ? _('Online') : _('Offline')
		}));

	if (item.vendor && !(item.isRandomized && item.vendor === _('Randomized')))
		metaItems.push(E('span', { 'class': 'netmon-host-vendor' }, item.vendor));

	if (item.isRandomized && (!item.vendor || item.vendor.indexOf(_('Randomized')) === -1))
		metaItems.push(E('span', { 'class': 'netmon-host-random' }, _('Randomized')));

	return E('div', { 'class': 'netmon-name-wrap' }, [
		E('div', { 'class': 'netmon-name-row' }, [ nameText, editIcon ]),
		metaItems.length ? E('div', { 'class': 'netmon-host-meta-line' }, metaItems) : ''
	]);
}

function replaceRows(table, rows, colspan, emptyText) {
	var oldRows = table.querySelectorAll('.tr:not(.table-titles)');
	for (var i = 0; i < oldRows.length; i++)
		oldRows[i].remove();

	if (rows.length) {
		rows.forEach(function(r) { table.appendChild(r); });
	}
	else {
		table.appendChild(E('tr', { 'class': 'tr placeholder netmon-placeholder' }, [
			E('td', { 'class': 'td', 'colspan': colspan }, E('em', emptyText))
		]));
	}
}

function showPlaceholder(table, colspan, text) {
	replaceRows(table, [], colspan, text);
}

return baseclass.extend({
	buildDevices: buildDevices,
	filterDevices: filterDevices,
	formatSpeed: formatSpeed,
	formatTraffic: formatTraffic,
	getPeriodStart: getPeriodStart,
	makeHostCell: makeHostCell,
	makeMetric: makeMetric,
	prepareVendorDb: prepareVendorDb,
	aggregateDevices: aggregateDevices,
	replaceRows: replaceRows,
	setText: setText,
	showPlaceholder: showPlaceholder,
	sortDevices: sortDevices,
	nextSortState: nextSortState,
	toSortableNumber: toSortableNumber
});
