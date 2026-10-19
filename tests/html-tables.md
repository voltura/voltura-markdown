# Voltura native HTML table model

## Footer ordering with and without spans

<table>
<tfoot><tr><td>Footer without spans</td></tr></tfoot>
<tbody><tr><td>Body without spans</td></tr></tbody>
</table>

<table>
<tfoot><tr><td colspan="2">Footer with spans</td></tr></tfoot>
<tbody><tr><td colspan="2">Body with spans</td></tr></tbody>
</table>

## Combined spans

<table>
<caption>Combined spans and row-group boundaries</caption>
<thead><tr><th colspan="3">Weekly activity</th></tr></thead>
<tbody>
<tr><th rowspan="2">Monday</th><td>Morning</td><td>Planning</td></tr>
<tr><td colspan="2" align="center">Afternoon: development and review</td></tr>
</tbody>
<tbody>
<tr><th rowspan="0">Tuesday</th><td>Morning</td><td>Implementation</td></tr>
<tr><td>Afternoon</td><td>Verification</td></tr>
<tr><td colspan="2" align="right">Group total: 3 sessions</td></tr>
</tbody>
<tfoot><tr><th colspan="2">Total sessions</th><td>5</td></tr></tfoot>
</table>

<table>
<tr><td rowspan="2" valign="middle" width="34%"><img src="../assets/icon-master.png" alt="Voltura icon" width="96"><br><sub>Spanning image and caption</sub></td><td>First row</td></tr>
<tr><td><table><tr><th colspan="2">Nested table</th></tr><tr><td>Left</td><td>Right</td></tr></table></td></tr>
</table>

## Standard recovery rules

<table>
<tr><th colspan="0">Zero column span defaults to one</th><td colspan="2suffix">Numeric prefix spans two columns</td></tr>
<tr><td rowspan="bad">Invalid row span defaults to one</td><td>Second</td><td>Third</td></tr>
</table>

Text after the tables remains ordinary **Markdown**, with [heading navigation](#voltura-native-html-table-model).

## Declared column grid

<table>
<colgroup span="8"><col span="2" width="25%"><col width="50%"></colgroup>
<thead><tr><th colspan="3">Declared 25 / 25 / 50 percent columns</th></tr></thead>
<tbody><tr><td>First quarter</td><td>Second quarter</td><td>Remaining half</td></tr></tbody>
</table>
