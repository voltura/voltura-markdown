# Code blocks

Hover over a copy icon, then copy the complete block. Try both themes and zoom levels.

```powershell
# Install and run
$project = "voltura-air"
git clone https://github.com/voltura/voltura-air.git
cd $project
npm ci
npm run dev
```

```typescript
interface Station { name: string; bitrate: number }
const station: Station = { name: "Voltura", bitrate: 320 };
// A typed function
export function label(value: Station): string {
  return `${value.name}: ${value.bitrate}`;
}
```

```javascript
async function loadStations(url) {
  const response = await fetch(url);
  return response.json();
}
```

```csharp
public record Station(string Name, int Bitrate);
// Keep Unicode and indentation when copying.
var station = new Station("Voltura — åäö", 320);
Console.WriteLine(station.Name);
```

```python
def greet(name):
    """A greeting."""
    return "Hello " + name
```

```json
{"name": "Voltura", "enabled": true, "count": 42}
```

```sql
SELECT name FROM stations WHERE bitrate >= 128;
```

```bash
if [ -n "$HOME" ]; then
  echo "$HOME"
fi
```

```unknown
This remains plain text: const value = 42;
```

```
Unlabeled blocks can also be copied.
```

```
```
