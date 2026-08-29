setupRoutes() {
    // Health check
    this.app.get('/health', (req, res) => {
        res.json({ status: 'healthy', service: 'ZenRift v2' });
    });

    // --- NUEVOS ENDPOINTS CRUD ---

    // GET /api/items?status=active&search=gateway
    this.app.get('/api/items', async (req, res) => {
        try {
            const filters = req.query; // { status, priority, search }
            const data = await this.service.getItems(filters);
            res.json({ success: true, count: data.length, data });
        } catch (error) {
            res.status(500).json({ success: false, error: error.message });
        }
    });

    // GET /api/items/:id
    this.app.get('/api/items/:id', async (req, res) => {
        try {
            const data = await this.service.getItemById(req.params.id);
            res.json({ success: true, data });
        } catch (error) {
            const status = error.code === 'NOT_FOUND' ? 404 : 500;
            res.status(status).json({ success: false, error: error.message });
        }
    });

    // POST /api/items
    this.app.post('/api/items', async (req, res) => {
        try {
            const newItem = await this.service.createItem(req.body);
            res.status(201).json({ success: true, data: newItem });
        } catch (error) {
            const status = error.code === 'VALIDATION_ERROR' ? 400 : 500;
            res.status(status).json({
                success: false,
                error: error.message,
                details: error.details || null,
            });
        }
    });

    // PUT /api/items/:id (actualización parcial)
    this.app.put('/api/items/:id', async (req, res) => {
        try {
            const updated = await this.service.updateItem(req.params.id, req.body);
            res.json({ success: true, data: updated });
        } catch (error) {
            const status = error.code === 'NOT_FOUND' ? 404 :
                           error.code === 'VALIDATION_ERROR' ? 400 : 500;
            res.status(status).json({
                success: false,
                error: error.message,
                details: error.details || null,
            });
        }
    });

    // DELETE /api/items/:id
    this.app.delete('/api/items/:id', async (req, res) => {
        try {
            const result = await this.service.deleteItem(req.params.id);
            res.json({ success: true, data: result });
        } catch (error) {
            const status = error.code === 'NOT_FOUND' ? 404 : 500;
            res.status(status).json({ success: false, error: error.message });
        }
    });

    // POST /api/process (mejorado)
    this.app.post('/api/process', async (req, res) => {
        try {
            const result = await this.service.process(req.body);
            res.json({ success: true, result });
        } catch (error) {
            const status = error.message.includes('requerido') || error.message.includes('soportada') ? 400 : 500;
            res.status(status).json({ success: false, error: error.message });
        }
    });

    // DELETE /api/cache (limpiar caché manual)
    this.app.delete('/api/cache', async (req, res) => {
        try {
            const result = await this.service.purgeCache();
            res.json({ success: true, result });
        } catch (error) {
            res.status(500).json({ success: false, error: error.message });
        }
    });

    // 404 - Fallback
    this.app.use((req, res) => {
        res.status(404).json({ error: 'Route not found' });
    });
}
