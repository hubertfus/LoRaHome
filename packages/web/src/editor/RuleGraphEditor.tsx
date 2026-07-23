import { useCallback, useMemo } from 'react';
import ReactFlow, {
  addEdge,
  Background,
  Connection,
  Controls,
  Edge,
  MiniMap,
  Node,
  useEdgesState,
  useNodesState,
} from 'reactflow';
import 'reactflow/dist/style.css';

const INITIAL_NODES: Node[] = [
  { id: 'sensor-1', position: { x: 0, y: 0 }, data: { label: 'bme680 · temperature' }, type: 'input' },
  { id: 'rule-1', position: { x: 250, y: 0 }, data: { label: 'GT 25.5°C (hyst 0.5, debounce 5s)' } },
  { id: 'action-1', position: { x: 500, y: 0 }, data: { label: 'action: relay_1 = ON' }, type: 'output' },
];

const INITIAL_EDGES: Edge[] = [
  { id: 'sensor-1->rule-1', source: 'sensor-1', target: 'rule-1' },
  { id: 'rule-1->action-1', source: 'rule-1', target: 'action-1' },
];

/**
 * Visual rule graph editor. This is the shape that the Host's Graph→CBOR
 * compiler (packages/host/src/compiler) consumes — nodes reduce down to
 * Rule[] records, never to arbitrary code. See ARCHITECTURE.md §6.
 */
export function RuleGraphEditor() {
  const [nodes, setNodes, onNodesChange] = useNodesState(INITIAL_NODES);
  const [edges, setEdges, onEdgesChange] = useEdgesState(INITIAL_EDGES);

  const onConnect = useCallback(
    (connection: Connection) => setEdges((eds) => addEdge(connection, eds)),
    [setEdges],
  );

  const style = useMemo(() => ({ width: '100%', height: '480px' }), []);

  return (
    <div style={style}>
      <ReactFlow
        nodes={nodes}
        edges={edges}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        onConnect={onConnect}
        fitView
      >
        <Background />
        <Controls />
        <MiniMap />
      </ReactFlow>
    </div>
  );
}
