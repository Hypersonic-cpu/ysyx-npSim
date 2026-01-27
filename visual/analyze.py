import os
import glob
import json
import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots

def load_data():
    data = []
    files = glob.glob('simout/sweep-mem40_10/*.json')
    
    for f in files:
        try:
            with open(f, 'r') as fd:
                j = json.load(fd)
                cfg = j['config']
                stats = j['stats0']
                
                # Parse Config
                ic_size = cfg['iCache']['size']
                ic_assoc = cfg['iCache']['assoc']
                ic_bs = cfg['iCache']['blkSize']
                
                # Handle BPU/BTB entries
                bpu_entries = 0
                btb_entries = 0
                
                # Check for "nobp" in filename to force 0 entries (Scenario requirement)
                is_nobp = 'nobp' in f
                
                if not is_nobp:
                    # Typo handling for BinmodalBP
                    if 'BinmodalBP' in cfg:
                        bpu_entries = cfg['BinmodalBP']['entries']
                    elif 'BimodalBP' in cfg:
                        bpu_entries = cfg['BimodalBP']['entries']
                        
                    if 'BTB' in cfg:
                        btb_entries = cfg['BTB']['entries']
                
                # Identify Scenario for Task 1
                scenario = 'Other'
                if is_nobp:
                    scenario = 'No BPU'
                elif bpu_entries == 32 and btb_entries == 16:
                    scenario = 'Medium BPU'
                elif bpu_entries == 64 and btb_entries == 64:
                    scenario = 'Strong BPU'
                
                ipc = stats['Pipeline']['ipc']
                
                # Area Model (Nangate 45nm & SyncReadMem)
                # 1. iCache Area
                capacity_bits = ic_size * 8
                area_ic = (capacity_bits * 0.8) * 1.2
                
                # 2. BPU Area
                area_bpu = bpu_entries * 2 * 5
                
                # 3. BTB Area
                # Target buffer: 32bit PC + 10bit Tag? (Assuming 10 from prompt)
                area_btb = btb_entries * (32 + 10) * 5
                
                total_area = area_ic + area_bpu + area_btb
                
                data.append({
                    'filename': f,
                    'icache_size': ic_size,
                    'assoc': ic_assoc,
                    'block_size': ic_bs,
                    'bpu_entries': bpu_entries,
                    'btb_entries': btb_entries,
                    'ipc': ipc,
                    'total_area': total_area,
                    'scenario': scenario
                })
                
        except Exception as e:
            print(f"Skipping {f}: {e}")
            
    return pd.DataFrame(data)

def create_task1_charts(df):
    html_parts = []
    
    scenarios = ['No BPU', 'Medium BPU', 'Strong BPU']
    titles = ['No BPU', 'Medium BPU (32/16)', 'Strong BPU (64/64)']
    
    for assoc in [1, 2]:
        fig = make_subplots(
            rows=1, cols=3,
            specs=[[{'type': 'surface'}, {'type': 'surface'}, {'type': 'surface'}]],
            subplot_titles=titles,
            horizontal_spacing=0.05
        )
        
        for i, scen in enumerate(scenarios):
            d = df[(df['assoc'] == assoc) & (df['scenario'] == scen)]
            if d.empty:
                continue
                
            # Pivot for 3D Surface
            # X: BlockSize, Y: iCache Size, Z: IPC
            p_ipc = d.pivot_table(index='icache_size', columns='block_size', values='ipc')
            p_area = d.pivot_table(index='icache_size', columns='block_size', values='total_area')
            
            # Ensure sorting
            p_ipc.sort_index(inplace=True)
            p_ipc.sort_index(axis=1, inplace=True)
            p_area.sort_index(inplace=True)
            p_area.sort_index(axis=1, inplace=True)
            
            fig.add_trace(
                go.Surface(
                    z=p_ipc.values,
                    x=p_ipc.columns,
                    y=p_ipc.index,
                    surfacecolor=p_area.values,
                    colorscale='Viridis',
                    colorbar=dict(title='Area (um^2)', x=1.00),
                    showscale=(i==2), # Only show colorbar on last plot to save space
                    name=scen
                ),
                row=1, col=i+1
            )
            
            fig.update_scenes(
                xaxis_title='Block Size',
                yaxis_title='iCache Size',
                zaxis_title='IPC',
                row=1, col=i+1
            )
            
        fig.update_layout(
            title_text=f'Task 1: iCache Area-Performance Sensitivity (Assoc={assoc})',
            height=600, width=1600,
            margin=dict(l=20, r=20, t=50, b=20)
        )
        html_parts.append(fig.to_html(full_html=False, include_plotlyjs='cdn'))
        
    return html_parts

def create_task2_chart(df):
    # Fixed: iCache=512B, BlockSize=16, Assoc=1
    d = df[(df['icache_size'] == 512) & (df['block_size'] == 16) & (df['assoc'] == 1)]
    
    if d.empty:
        return "<div>No data for Task 2</div>"
        
    # Pivot
    # X=BPU Entries, Y=BTB Entries, Z=IPC
    p_ipc = d.pivot_table(index='btb_entries', columns='bpu_entries', values='ipc')
    p_area = d.pivot_table(index='btb_entries', columns='bpu_entries', values='total_area')
    
    p_ipc.sort_index(inplace=True)
    p_ipc.sort_index(axis=1, inplace=True)
    p_area.sort_index(inplace=True)
    p_area.sort_index(axis=1, inplace=True)
    
    fig = go.Figure()
    
    # Heatmap (IPC)
    fig.add_trace(go.Heatmap(
        z=p_ipc.values,
        x=p_ipc.columns,
        y=p_ipc.index,
        colorscale='Plasma',
        colorbar=dict(title='IPC'),
        hovertemplate='BPU: %{x}<br>BTB: %{y}<br>IPC: %{z:.4f}<extra></extra>'
    ))
    
    # Contour (Area)
    fig.add_trace(go.Contour(
        z=p_area.values,
        x=p_area.columns,
        y=p_area.index,
        colorscale='Greys',
        contours=dict(
            coloring='lines',
            showlabels=True,
            labelfont=dict(size=12, color='white')
        ),
        line=dict(width=2, color='white'),
        showscale=False,
        hovertemplate='Area: %{z:.1f} um^2<extra></extra>'
    ))
    
    fig.update_layout(
        title='Task 2: Branch Predictor Resource Trade-off (iCache=512B, BS=16, Assoc=1)',
        xaxis_title='BPU Entries',
        yaxis_title='BTB Entries',
        height=700, width=900
    )
    
    return fig.to_html(full_html=False, include_plotlyjs='cdn')

def main():
    df = load_data()
    print(f"Loaded {len(df)} records.")
    
    t1_htmls = create_task1_charts(df)
    t2_html = create_task2_chart(df)
    
    with open('visual/report.html', 'w') as f:
        f.write('<html><head><title>DSE Analysis</title></head><body>')
        f.write('<h1>VLSI DSE Analysis Report</h1>')
        f.write('<h2>Task 1: iCache Area-Performance Sensitivity</h2>')
        for h in t1_htmls:
            f.write(h)
            f.write('<br>')
        f.write('<h2>Task 2: Branch Predictor Resource Trade-off</h2>')
        f.write(t2_html)
        f.write('</body></html>')
        
    print("Report generated at visual/report.html")

if __name__ == "__main__":
    main()
